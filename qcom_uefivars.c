#include <linux/dma-mapping.h>
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

static void qseos_dma_aligned(const struct qseos_dma *base, struct qseos_dma *out,
			      u64 offset, u64 align)
{
	out->virt = (void *)ALIGN((uintptr_t)base->virt + offset, align);
	out->phys = base->phys + (out->virt - base->virt);
	out->size = base->size - (out->virt - base->virt);
}


/* -- UTF-16 helpers. ------------------------------------------------------- */

static u64 utf16_strnlen(wchar_t* str, u64 max)
{
	u64 i;

	for (i = 0; *str != 0 && i < max; i++, str++) {
		/* Do nothing, all is handled in the for statement. */
	}

	return i;
}


/* -- TODO. ----------------------------------------------------------------- */

#define MAX_APP_NAME_SIZE		64

#define TZ_OWNER_TZ_APPS		48
#define TZ_OWNER_QSEE_OS		50

#define TZ_SVC_APP_ID_PLACEHOLDER	0
#define TZ_SVC_APP_MGR			1
#define TZ_SVC_LISTENER			2

#define TZ_UEFI_VAR_CMD(x)		(0x8000 | x)
#define TZ_UEFI_VAR_GET_VARIABLE	TZ_UEFI_VAR_CMD(0)
#define TZ_UEFI_VAR_SET_VARIABLE	TZ_UEFI_VAR_CMD(1)
#define TZ_UEFI_VAR_GET_NEXT_VARIABLE	TZ_UEFI_VAR_CMD(2)
#define TZ_UEFI_VAR_QUERY_VARIABLE_INFO	TZ_UEFI_VAR_CMD(3)

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

struct qcom_uefi_get_next_variable_name_req {
	u32 command_id;
	u32 length;
	u32 guid_offset;
	u32 guid_size;
	u32 name_offset;
	u32 name_size;
} __packed;

struct qcom_uefi_get_next_variable_name_rsp {
	u32 command_id;
	u32 length;
	u32 status;
	u32 guid_offset;
	u32 guid_size;
	u32 name_offset;
	u32 name_size;
} __packed;

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

	dev_info(dev, "%s: owner=%x, svc=%x, cmd=%x, status=%lld, type=%llx, data=%llx",
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

static int qseos_uefi_get_next_variable_name(struct device *dev, u32 app_id,
					     u64 *name_size, wchar_t* name, guid_t* guid)
{
	struct qcom_uefi_get_next_variable_name_req *req_data;
	struct qcom_uefi_get_next_variable_name_rsp *rsp_data;
	struct qseos_dma dma_base;
	struct qseos_dma dma_req;
	struct qseos_dma dma_rsp;
	u64 size = PAGE_SIZE;
	u64 input_name_len;
	int status;

	// size = (size + PAGE_SIZE) & PAGE_MASK;
	status = qseos_dma_alloc(dev, &dma_base, size, GFP_KERNEL);
	if (status)
		return status;

	qseos_dma_aligned(&dma_base, &dma_req, 0, __alignof__(*req_data));

	req_data = dma_req.virt;
	req_data->command_id = TZ_UEFI_VAR_GET_NEXT_VARIABLE;
	req_data->guid_offset = sizeof(*req_data);
	req_data->guid_size = sizeof(*guid);
	req_data->name_offset = req_data->guid_offset + req_data->guid_size;
	req_data->name_size = *name_size;
	req_data->length = req_data->name_offset + req_data->name_size;

	dma_req.size = req_data->length;

	input_name_len = utf16_strnlen(name, *name_size - 1);
	memcpy(dma_req.virt + req_data->guid_offset, guid, req_data->guid_size);
	memcpy(dma_req.virt + req_data->name_offset, name, input_name_len * sizeof(wchar_t));
	*(wchar_t *)(dma_req.virt + req_data->name_offset + (input_name_len + 1) * sizeof(wchar_t)) = 0;

	qseos_dma_aligned(&dma_base, &dma_rsp, req_data->length, __alignof__(*rsp_data));

	dma_wmb();
	status = qseos_app_send(dev, app_id, dma_req.phys, dma_req.size, dma_rsp.phys, dma_rsp.size);
	dma_rmb();

	if (status == 0) {
		rsp_data = dma_rsp.virt;

		if (rsp_data->status == 0) {
			memcpy(guid, dma_rsp.virt + rsp_data->guid_offset, rsp_data->guid_size);
			memcpy(name, dma_rsp.virt + rsp_data->name_offset, min((u32)*name_size, rsp_data->name_size));
			*name_size = rsp_data->name_size;
			name[*name_size - 1] = 0;
		}
	}

	qseos_dma_free(dev, &dma_base);

	if (status)
		return status;

	return 0;
}

static int qcom_uefivars_probe(struct platform_device *pdev)
{
	const char *app_name = "qcom.tz.uefisecapp";
	u32 app_id = U32_MAX;
	guid_t var_guid = {};
	wchar_t var_name[256] = {};
	char var_name_u8[256] = {};
	u64 var_size = ARRAY_SIZE(var_name) * sizeof(var_name[0]);
	int status;

	dev_info(&pdev->dev, "%s\n", __func__);

	if (dma_set_mask(&pdev->dev, DMA_BIT_MASK(64))) {
		dev_warn(&pdev->dev, "no suitable DMA available\n");
		return -EFAULT;
	}

	status = qseos_app_get_id(&pdev->dev, app_name, &app_id);
	if (status)
		return status;

	status = qseos_uefi_get_next_variable_name(&pdev->dev, app_id, &var_size,
						   var_name, &var_guid);
	if (status)
		return status;
	
	utf16s_to_utf8s(var_name, var_size, UTF16_LITTLE_ENDIAN, var_name_u8, ARRAY_SIZE(var_name_u8) - 1);
	dev_info(&pdev->dev, "%s: name=%s, guid=%pUL\n", __func__, var_name_u8, &var_guid);

	return 0;
}

static int qcom_uefivars_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "%s\n", __func__);
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
