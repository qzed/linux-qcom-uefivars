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
	if (status)
		return status;

	res->status = scm_res.result[0];
	res->resp_type = scm_res.result[1];
	res->data = scm_res.result[2];

	return 0;
}

static int qseos_syscall(struct device *dev, const struct qcom_scm_desc *desc, struct qseos_res *res)
{
	int status;

	status = __qseos_syscall(desc, res);
	if (status) {
		dev_err(dev, "qcom_scm_call failed with errro %d\n", status);
		return status;
	}

	dev_info(dev, "%s: owner=%x, svc=%x, cmd=%x, status=%llx, type=%llx, data=%llx",
		 __func__, desc->owner, desc->svc, desc->cmd, res->status,
		 res->resp_type, res->data);

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

	dev_info(dev, "%s: name=%s, id=%u\n", __func__, app_name, *app_id);
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

static u64 utf16_strnlen(wchar_t* str, u64 max)
{
	u64 i;

	for(i = 0; *str != 0 && i < max; i++, str++) {}

	return i;
}

static int qseos_uefi_get_next_variable_name(struct device *dev, u32 app_id,
					     u64 *name_size, wchar_t* name, guid_t* guid)
{
	struct qcom_uefi_get_next_variable_name_req *req_data;
	struct qcom_uefi_get_next_variable_name_rsp *rsp_data;
	u64 size = PAGE_SIZE;
	dma_addr_t buf_phys;
	dma_addr_t req_phys;
	dma_addr_t rsp_phys;
	void *buf_virt;
	void *req_virt;
	void *rsp_virt;
	u64 req_len;
	u64 rsp_len;
	int status;
	char name_u8[256] = {};

	// size = (size + PAGE_SIZE) & PAGE_MASK;
	buf_virt = dma_alloc_coherent(dev, size, &buf_phys, GFP_KERNEL);
	if (!buf_virt) {
		dev_err(dev, "%s: failed to allocate DMA memory\n", __func__);
		return -ENOMEM;
	}

	req_virt = (void *)ALIGN((u64)buf_virt, 32);
	req_phys = buf_phys + (req_virt - buf_virt);
	req_data = req_virt;

	req_data->command_id = TZ_UEFI_VAR_GET_NEXT_VARIABLE;
	req_data->guid_offset = sizeof(*req_data);
	req_data->guid_size = sizeof(*guid);
	req_data->name_offset = req_data->guid_offset + req_data->guid_size;
	req_data->name_size = *name_size;
	req_data->length = req_data->name_offset + req_data->name_size;

	memcpy(req_virt + req_data->guid_offset, guid, req_data->guid_size);
	memcpy(req_virt + req_data->name_offset, name, utf16_strnlen(name, *name_size));
	*(wchar_t *)(req_virt + req_data->name_offset + utf16_strnlen(name, *name_size)) = 0;

	rsp_virt = (void *)ALIGN((u64)buf_virt + req_data->length, 32);
	rsp_phys = buf_phys + (rsp_virt - buf_virt);
	rsp_data = rsp_virt;

	req_len = req_data->length;
	rsp_len = buf_virt + PAGE_SIZE - rsp_virt;

	dma_wmb();

	status = qseos_app_send(dev, app_id, req_phys, req_len, rsp_phys, rsp_len);

	dma_rmb();

	if (status == 0) {
		memcpy(guid, rsp_virt + rsp_data->guid_offset, rsp_data->guid_size);
		memcpy(name, rsp_virt + rsp_data->name_offset, min((u32)*name_size, rsp_data->name_size));
		*name_size = rsp_data->name_size;
		name[*name_size - 1] = 0;

		utf16s_to_utf8s(name, *name_size, UTF16_LITTLE_ENDIAN, name_u8, ARRAY_SIZE(name_u8) - 1);

		dev_info(dev, "%s: rsp.cmd=0x%x, rsp.status=0x%x, rsp.len=%u, name=%s, guid=%pUL\n",
			 __func__, rsp_data->command_id, rsp_data->status, rsp_data->length,
			 name_u8, guid);
	}

	dma_free_coherent(dev, size, buf_virt, buf_phys);

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
