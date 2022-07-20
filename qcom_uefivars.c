#include <linux/dma-mapping.h>
#include <linux/efi.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/qcom_scm.h>
#include <linux/slab.h>
#include <linux/uuid.h>
#include <linux/nls.h>


/* -- DMA helpers. ---------------------------------------------------------- */

struct qseos_dma {
	unsigned long size;
	void *virt;
	dma_addr_t phys;
};

static int qseos_dma_alloc(struct device *dev, struct qseos_dma *dma, u64 size, gfp_t gfp)
{
	dma->virt = dma_alloc_coherent(dev, size, &dma->phys, GFP_KERNEL);
	if (!dma->virt)
		return -ENOMEM;

	dma->size = size;
	return 0;
}

static void qseos_dma_free(struct device *dev, struct qseos_dma *dma)
{
	dma_free_coherent(dev, dma->size, dma->virt, dma->phys);
}

static int qseos_dma_realloc(struct device *dev, struct qseos_dma *dma, u64 size, gfp_t gfp)
{
	if (size <= dma->size)
		return 0;

	qseos_dma_free(dev, dma);
	return qseos_dma_alloc(dev, dma, size, gfp);
}

static void qseos_dma_aligned(const struct qseos_dma *base, struct qseos_dma *out,
			      u64 offset, u64 align)
{
	out->virt = (void *)ALIGN((uintptr_t)base->virt + offset, align);
	out->phys = base->phys + (out->virt - base->virt);
	out->size = base->size - (out->virt - base->virt);
}


/* -- UTF-16 helpers. ------------------------------------------------------- */

static u64 utf16_strnlen(const wchar_t* str, u64 max)
{
	u64 i;

	for (i = 0; *str != 0 && i < max; i++, str++) {
		/* Do nothing, all is handled in the for statement. */
	}

	return i;
}

static size_t utf16_strlcpy(wchar_t *dst, const wchar_t *src, size_t size)
{
	size_t actual = utf16_strnlen(src, size - 1);

	memcpy(dst, src, actual * sizeof(src[0]));
	dst[actual] = 0;

	return actual;
}


/* -- TzApp interface. ------------------------------------------------------ */

#define MAX_APP_NAME_SIZE		64

#define TZ_OWNER_TZ_APPS		48
#define TZ_OWNER_QSEE_OS		50

#define TZ_SVC_APP_ID_PLACEHOLDER	0
#define TZ_SVC_APP_MGR			1
#define TZ_SVC_LISTENER			2

enum qseecom_qseos_cmd_status {
	QSEOS_RESULT_SUCCESS = 0,
	QSEOS_RESULT_INCOMPLETE,
	QSEOS_RESULT_BLOCKED_ON_LISTENER,
	QSEOS_RESULT_FAILURE = 0xFFFFFFFF
};

enum qseecom_command_scm_resp_type {
	QSEOS_APP_ID = 0xEE01,
	QSEOS_LISTENER_ID
};

struct qseos_res {
	u64 status;
	u64 resp_type;
	u64 data;
};

static int __qseos_syscall(const struct qcom_scm_desc *desc, struct qseos_res *res)
{
	struct qcom_scm_res scm_res = {};
	int status;

	status = qcom_scm_call(desc, &scm_res);

	res->status = scm_res.result[0];
	res->resp_type = scm_res.result[1];
	res->data = scm_res.result[2];

	if (status)
		return status;

	return 0;
}

static int qseos_syscall(struct device *dev, const struct qcom_scm_desc *desc, struct qseos_res *res)
{
	int status;

	status = __qseos_syscall(desc, res);

	dev_dbg(dev, "%s: owner=%x, svc=%x, cmd=%x, status=%lld, type=%llx, data=%llx",
		__func__, desc->owner, desc->svc, desc->cmd, res->status,
		res->resp_type, res->data);

	if (status) {
		dev_err(dev, "qcom_scm_call failed with errro %d\n", status);
		return status;
	}

	// TODO: handle incomplete calls

	return 0;
}

static int qseos_app_get_id(struct device *dev, const char* app_name, u32 *app_id)
{
	u32 tzbuflen = MAX_APP_NAME_SIZE;
	struct qcom_scm_desc desc = {};
	struct qseos_res res = {};
	dma_addr_t addr_tzbuf;
	char *tzbuf;
	int status;

	tzbuf = kzalloc(tzbuflen, GFP_KERNEL);
	if (!tzbuf)
		return -ENOMEM;

	strlcpy(tzbuf, app_name, tzbuflen);

	addr_tzbuf = dma_map_single(dev, tzbuf, tzbuflen, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, addr_tzbuf)) {
		dev_err(dev, "failed to map dma address\n");
		return -EFAULT;
	}

	desc.owner = TZ_OWNER_QSEE_OS;
	desc.svc = TZ_SVC_APP_MGR;
	desc.cmd = 0x03;
	desc.arginfo = QCOM_SCM_ARGS(2, QCOM_SCM_RW, QCOM_SCM_VAL);
	desc.args[0] = addr_tzbuf;
	desc.args[1] = strlen(app_name);

	status = qseos_syscall(dev, &desc, &res);
	dma_unmap_single(dev, addr_tzbuf, tzbuflen, DMA_BIDIRECTIONAL);
	kfree(tzbuf);

	if (status)
		return status;

	if (res.status != QSEOS_RESULT_SUCCESS)
		return -EINVAL;

	*app_id = res.data;
	return 0;
}

static int qseos_app_send(struct device *dev, u32 app_id, dma_addr_t req,
			  u64 req_len, dma_addr_t rsp, u64 rsp_len)
{
	struct qseos_res res = {};
	int status;

	struct qcom_scm_desc desc = {
		.owner = TZ_OWNER_TZ_APPS,
		.svc = TZ_SVC_APP_ID_PLACEHOLDER,
		.cmd = 0x01,
		.arginfo = QCOM_SCM_ARGS(5, QCOM_SCM_VAL,
					 QCOM_SCM_RW, QCOM_SCM_VAL,
					 QCOM_SCM_RW, QCOM_SCM_VAL),
		.args[0] = app_id,
		.args[1] = req,
		.args[2] = req_len,
		.args[3] = rsp,
		.args[4] = rsp_len,
	};

	status = qseos_syscall(dev, &desc, &res);
	if (status)
		return status;

	if (res.status != QSEOS_RESULT_SUCCESS)
		return -EFAULT;

	return 0;
}


/* -- UEFI app interface. --------------------------------------------------- */

#define QCOM_UEFISEC_APP_NAME	"qcom.tz.uefisecapp"

struct qcom_uefi_app {
	struct device *dev;
	struct qseos_dma dma;
	u32 app_id;
};

#define TZ_UEFI_VAR_CMD(x)		(0x8000 | x)
#define TZ_UEFI_VAR_GET_VARIABLE	TZ_UEFI_VAR_CMD(0)
#define TZ_UEFI_VAR_SET_VARIABLE	TZ_UEFI_VAR_CMD(1)
#define TZ_UEFI_VAR_GET_NEXT_VARIABLE	TZ_UEFI_VAR_CMD(2)
#define TZ_UEFI_VAR_QUERY_VARIABLE_INFO	TZ_UEFI_VAR_CMD(3)

struct qcom_uefi_get_next_variable_name_req {
	u32 command_id;
	u32 length;
	u32 guid_offset;
	u32 guid_size;
	u32 name_offset;
	u32 name_size;		/* Size of full buffer in bytes with nul-terminator. */
} __packed;

struct qcom_uefi_get_next_variable_name_rsp {
	u32 command_id;
	u32 length;
	u32 status;
	u32 guid_offset;
	u32 guid_size;
	u32 name_offset;
	u32 name_size;		/* Size in bytes with nul-terminator. */
} __packed;

static efi_status_t qseos_uefi_status_to_efi(u32 status)
{
	u64 category = status & 0xf0000000;
	u64 code = status & 0x0fffffff;

	return category << (BITS_PER_LONG - 32) | code;
}

// NOTE: adapted from kernel, not required in final driver
int __efi_status_to_err(efi_status_t status)
{
	int err;

	switch (status) {
	case EFI_SUCCESS:
		err = 0;
		break;
	case EFI_INVALID_PARAMETER:
		err = -EINVAL;
		break;
	case EFI_OUT_OF_RESOURCES:
		err = -ENOSPC;
		break;
	case EFI_DEVICE_ERROR:
		err = -EIO;
		break;
	case EFI_WRITE_PROTECTED:
		err = -EROFS;
		break;
	case EFI_SECURITY_VIOLATION:
		err = -EACCES;
		break;
	case EFI_NOT_FOUND:
		err = -ENOENT;
		break;
	case EFI_ABORTED:
		err = -EINTR;
		break;
	case EFI_BUFFER_TOO_SMALL:
		err = -E2BIG;
		break;
	default:
		err = -EINVAL;
	}

	return err;
}

static int qcuefi_get_next_variable_name(struct qcom_uefi_app *qcuefi, u64 *name_size,
					 wchar_t* name, efi_guid_t* guid)
{
	struct qcom_uefi_get_next_variable_name_req *req_data;
	struct qcom_uefi_get_next_variable_name_rsp *rsp_data;
	struct qseos_dma dma_req;
	struct qseos_dma dma_rsp;
	efi_status_t efi_status;
	int status;
	u64 size;

	/* Compute required size. */
	size = sizeof(*req_data) + sizeof(*guid) + *name_size;    /* Inputs.            */
	size += sizeof(*rsp_data) + sizeof(*guid) + *name_size;   /* Outputs.           */
	size += __alignof__(*req_data) + __alignof__(*guid);      /* Input alignments.  */
	size += __alignof__(*rsp_data);                           /* Output alignments. */
	size = PAGE_ALIGN(size);

	/* Make sure we have enough DMA memory. */
	status = qseos_dma_realloc(qcuefi->dev, &qcuefi->dma, size, GFP_KERNEL);
	if (status)
		return status;

	/* Align request struct. */
	qseos_dma_aligned(&qcuefi->dma, &dma_req, 0, __alignof__(*req_data));
	req_data = dma_req.virt;

	/* Set up request data. */
	req_data->command_id = TZ_UEFI_VAR_GET_NEXT_VARIABLE;
	req_data->guid_offset = ALIGN(sizeof(*req_data), __alignof__(*guid));
	req_data->guid_size = sizeof(*guid);
	req_data->name_offset = req_data->guid_offset + req_data->guid_size;
	req_data->name_size = *name_size;
	req_data->length = req_data->name_offset + req_data->name_size;

	dma_req.size = req_data->length;

	/* Copy request parameters. */
	memcpy(dma_req.virt + req_data->guid_offset, guid, req_data->guid_size);
	utf16_strlcpy(dma_req.virt + req_data->name_offset, name, *name_size / sizeof(wchar_t));

	/* Align response struct. */
	qseos_dma_aligned(&qcuefi->dma, &dma_rsp, req_data->length, __alignof__(*rsp_data));
	rsp_data = dma_rsp.virt;

	/* Perform SCM call. */
	dma_wmb();
	status = qseos_app_send(qcuefi->dev, qcuefi->app_id, dma_req.phys, dma_req.size,
				dma_rsp.phys, dma_rsp.size);
	dma_rmb();

	/* Check for errors and validate. */
	if (status)
		return status;

	if (rsp_data->status) {
		dev_err(qcuefi->dev, "%s: uefisecapp error: 0x%x\n", __func__, rsp_data->status);
		efi_status = qseos_uefi_status_to_efi(rsp_data->status);

		/* Update size with required size in case buffer is too small. */
		if (efi_status == EFI_BUFFER_TOO_SMALL)
			*name_size = rsp_data->name_size;

		return __efi_status_to_err(efi_status);
	}

	if (rsp_data->guid_size != sizeof(*guid))
		return -EPROTO;

	if (rsp_data->name_size > *name_size)
		return -E2BIG;

	/* Copy response fields. */
	memcpy(guid, dma_rsp.virt + rsp_data->guid_offset, rsp_data->guid_size);
	utf16_strlcpy(name, dma_rsp.virt + rsp_data->name_offset, rsp_data->name_size / sizeof(wchar_t));
	*name_size = rsp_data->name_size;

	return 0;
}

static int _qcuefi_get_and_print_next(struct qcom_uefi_app *qcuefi, u64 *name_size,
				      wchar_t* name, efi_guid_t* guid)
{
	char name_u8[256] = {};
	int status;

	status = qcuefi_get_next_variable_name(qcuefi, name_size, name, guid);
	if (status) {
		dev_err(qcuefi->dev, "%s: failed to read variable: status=%d\n", __func__, status);
		return status;
	}

	utf16s_to_utf8s(name, *name_size, UTF16_LITTLE_ENDIAN, name_u8, ARRAY_SIZE(name_u8) - 1);
	dev_info(qcuefi->dev, "%s: name=%s, guid=%pUL\n", __func__, name_u8, guid);

	return 0;
}

static int _qcuefi_test(struct qcom_uefi_app *qcuefi)
{
	wchar_t var_name[256] = {};
	efi_guid_t var_guid = {};
	u64 var_size;
	int status, i;

	for (i = 0; i < 100; i++) {
		var_size = ARRAY_SIZE(var_name) * sizeof(var_name[0]);
		status = _qcuefi_get_and_print_next(qcuefi, &var_size, var_name, &var_guid);
		if (status == -ENOENT) {
			dev_info(qcuefi->dev, "end of variables reached\n");
			break;
		}
		if (status)
			return status;
	}

	return 0;
}

static int qcom_uefivars_probe(struct platform_device *pdev)
{
	struct qcom_uefi_app *qcuefi;
	int status;

	/* Allocate driver data. */
	qcuefi = devm_kzalloc(&pdev->dev, sizeof(*qcuefi), GFP_KERNEL);
	if (!qcuefi)
		return -ENOMEM;

	qcuefi->dev = &pdev->dev;

	/* Get application id for uefisecapp. */
	status = qseos_app_get_id(&pdev->dev, QCOM_UEFISEC_APP_NAME, &qcuefi->app_id);
	if (status)
		return status;

	/* Set up DMA. One page should be plenty to start with. */
	if (dma_set_mask(&pdev->dev, DMA_BIT_MASK(64))) {
		dev_warn(&pdev->dev, "no suitable DMA available\n");
		return -EFAULT;
	}

	status = qseos_dma_alloc(&pdev->dev, &qcuefi->dma, PAGE_SIZE, GFP_KERNEL);
	if (status)
		return status;

	platform_set_drvdata(pdev, qcuefi);

	/* Run tests. */
	status = _qcuefi_test(qcuefi);
	if (status)
		goto err;

	return 0;

err:
	qseos_dma_free(qcuefi->dev, &qcuefi->dma);
	return status;
}

static int qcom_uefivars_remove(struct platform_device *pdev)
{
	struct qcom_uefi_app *qcuefi = platform_get_drvdata(pdev);

	qseos_dma_free(qcuefi->dev, &qcuefi->dma);
	return 0;
}

static struct platform_driver qcom_uefivars_driver = {
	.probe = qcom_uefivars_probe,
	.remove = qcom_uefivars_remove,
	.driver = {
		.name = "qcom_uefivars",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};


/* -- Module initialization. ------------------------------------------------ */

static struct platform_device *qcom_uefivars_device;

static int __init qcom_uefivars_init(void)
{
	struct platform_device *pdev;
	int status;

	status = platform_driver_register(&qcom_uefivars_driver);
	if (status)
		return status;

	pdev = platform_device_alloc("qcom_uefivars", PLATFORM_DEVID_NONE);
	if (!pdev) {
		status = -ENOMEM;
		goto err_alloc;
	}

	status = platform_device_add(pdev);
	if (status)
		goto err_add;

	qcom_uefivars_device = pdev;
	return 0;

err_add:
	platform_device_put(pdev);
err_alloc:
	platform_driver_unregister(&qcom_uefivars_driver);
	return status;
}
module_init(qcom_uefivars_init);

static void __exit qcom_uefivars_exit(void)
{
	platform_device_unregister(qcom_uefivars_device);
	platform_driver_unregister(&qcom_uefivars_driver);
}
module_exit(qcom_uefivars_exit);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("Test");
MODULE_LICENSE("GPL");
