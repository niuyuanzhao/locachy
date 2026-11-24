// SPDX-License-Identifier: GPL-2.0-only
/*
 * hp_omen_gpu -- Read/write HP OMEN BIOS dGPU power modes (cTGP / PPAB / D-state).
 *
 * Uses the exact same HP WMI BIOS query as upstream drivers/platform/x86/hp/hp-wmi.c
 * victus_s_gpu_thermal_profile_get()/set():
 *   GUID 5FB7F034-2C63-45E9-BE91-3D44E2C707E4, command HPWMI_GM (0x20008),
 *   query 0x21 (read) / 0x22 (write), payload:
 *     { u8 ctgp_enable, ppab_enable, dstate, gpu_slowdown_temp }.
 *
 * Upstream hp-wmi restricts this call to a Victus S board allowlist, so OMEN
 * 17-ck1xxx (board 8A1A) leaves the dGPU capped at 115 W even when
 * platform_profile=performance.
 *
 * Safety rule (matching upstream): gpu_slowdown_temp is always read back from
 * BIOS and written verbatim so thermal protection thresholds are never altered.
 * Parameter -1 keeps the current BIOS value; all -1 performs a read-only query.
 * No MODULE_ALIAS is exported, and no daemon/timer stays resident.
 */
#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/wmi.h>

#define HPWMI_BIOS_GUID "5FB7F034-2C63-45E9-BE91-3D44E2C707E4"
#define HPWMI_GM 0x20008
#define HPWMI_GET_GPU_THERMAL_MODES_QUERY 0x21
#define HPWMI_SET_GPU_THERMAL_MODES_QUERY 0x22
#define HPWMI_SIGNATURE 0x55434553 /* "SECU" */
#define HPWMI_RET_UNKNOWN_COMMAND 0x03
#define HPWMI_RET_UNKNOWN_CMDTYPE 0x04

struct bios_args {
	u32 signature;
	u32 command;
	u32 commandtype;
	u32 datasize;
	u8 data[];
};

struct bios_return {
	u32 sigpass;
	u32 return_code;
};

struct gpu_power_modes {
	u8 ctgp_enable;
	u8 ppab_enable;
	u8 dstate;
	u8 gpu_slowdown_temp;
};

static int ctgp = -1;
module_param(ctgp, int, 0444);
MODULE_PARM_DESC(ctgp, "custom TGP: 1=on 0=off, -1=keep (default)");

static int ppab = -1;
module_param(ppab, int, 0444);
MODULE_PARM_DESC(ppab, "PPAB / Dynamic Boost: 1=on 0=off, -1=keep (default)");

static int dstate = -1;
module_param(dstate, int, 0444);
MODULE_PARM_DESC(dstate, "GPU D-state: 1=100% 2=50% 3=25% 4=12.5%, -1=keep (default)");

static int hp_bios_gpu_query(int commandtype, struct gpu_power_modes *buf, int outsize)
{
	struct acpi_buffer input, output = { ACPI_ALLOCATE_BUFFER, NULL };
	const int insize = sizeof(*buf);
	struct bios_return *br;
	union acpi_object *obj;
	struct bios_args *args;
	size_t args_size;
	acpi_status status;
	int ret;

	args_size = struct_size(args, data, 128);
	args = kzalloc(args_size, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	args->signature = HPWMI_SIGNATURE;
	args->command = HPWMI_GM;
	args->commandtype = commandtype;
	args->datasize = insize;
	memcpy(args->data, buf, insize);

	input.length = args_size;
	input.pointer = args;

	status = wmi_evaluate_method(HPWMI_BIOS_GUID, 0, outsize ? 2 : 1, &input, &output);
	kfree(args);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER || obj->buffer.length < sizeof(*br)) {
		kfree(obj);
		return -EINVAL;
	}

	br = (struct bios_return *)obj->buffer.pointer;
	ret = br->return_code;
	if (ret == 0 && outsize) {
		if (obj->buffer.length - sizeof(*br) < (size_t)outsize)
			ret = -EMSGSIZE;
		else
			memcpy(buf, (u8 *)obj->buffer.pointer + sizeof(*br), outsize);
	}
	kfree(obj);
	return ret;
}

static void show(const char *when, const struct gpu_power_modes *m)
{
	pr_info("hp_omen_gpu: %s ctgp=%u ppab=%u dstate=%u gpu_slowdown_temp=%u\n",
		when, m->ctgp_enable, m->ppab_enable, m->dstate, m->gpu_slowdown_temp);
}

static int __init hp_omen_gpu_init(void)
{
	struct gpu_power_modes cur = {}, want, after = {};
	int ret;

	if (!wmi_has_guid(HPWMI_BIOS_GUID)) {
		pr_err("hp_omen_gpu: HP BIOS WMI interface not present\n");
		return -ENODEV;
	}
	if (ctgp < -1 || ctgp > 1 || ppab < -1 || ppab > 1 ||
	    dstate < -1 || dstate == 0 || dstate > 4) {
		pr_err("hp_omen_gpu: bad parameter (ctgp/ppab: -1|0|1, dstate: -1|1..4)\n");
		return -EINVAL;
	}

	ret = hp_bios_gpu_query(HPWMI_GET_GPU_THERMAL_MODES_QUERY, &cur, sizeof(cur));
	if (ret) {
		pr_err("hp_omen_gpu: read failed (%d)%s\n", ret,
		       (ret == HPWMI_RET_UNKNOWN_COMMAND || ret == HPWMI_RET_UNKNOWN_CMDTYPE) ?
		       ": this BIOS does not support the GPU power query" : "");
		return ret < 0 ? ret : -EOPNOTSUPP;
	}
	show("before:", &cur);

	if (ctgp == -1 && ppab == -1 && dstate == -1)
		return 0;

	want = cur;
	if (ctgp != -1)
		want.ctgp_enable = ctgp;
	if (ppab != -1)
		want.ppab_enable = ppab;
	if (dstate != -1)
		want.dstate = dstate;
	want.gpu_slowdown_temp = cur.gpu_slowdown_temp;

	ret = hp_bios_gpu_query(HPWMI_SET_GPU_THERMAL_MODES_QUERY, &want, 0);
	if (ret) {
		pr_err("hp_omen_gpu: write failed (%d)\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	ret = hp_bios_gpu_query(HPWMI_GET_GPU_THERMAL_MODES_QUERY, &after, sizeof(after));
	if (ret) {
		pr_warn("hp_omen_gpu: written, but read-back failed (%d)\n", ret);
		return 0;
	}
	show("after: ", &after);
	if (after.gpu_slowdown_temp != cur.gpu_slowdown_temp)
		pr_warn("hp_omen_gpu: gpu_slowdown_temp changed %u -> %u\n",
			cur.gpu_slowdown_temp, after.gpu_slowdown_temp);
	return 0;
}

static void __exit hp_omen_gpu_exit(void)
{
}

module_init(hp_omen_gpu_init);
module_exit(hp_omen_gpu_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read/set HP OMEN BIOS GPU power modes (cTGP/PPAB/D-state), preserving gpu_slowdown_temp");
