#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/qcom_scm.h>
#include <linux/slab.h>

#define MAX_APP_NAME_SIZE		64

#define TZ_OWNER_TZ_APPS		48
#define TZ_OWNER_QSEE_OS		50

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

static int qcom_uefivars_probe(struct platform_device *pdev)
{
	const char *app_name = "qcom.tz.uefisecapp";
	u32 app_id = U32_MAX;
	int status;

	dev_info(&pdev->dev, "%s\n", __func__);

	status = qseos_app_get_id(&pdev->dev, app_name, &app_id);
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
