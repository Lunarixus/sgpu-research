/*
 * Copyright 2019-2023 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/pci.h>
#ifdef CONFIG_DRM_SGPU_EXYNOS
#include <soc/samsung/exynos-smc.h>
#include <soc/samsung/exynos/exynos-soc.h>
#if IS_ENABLED(CONFIG_EXYNOS_IMGLOADER)
#include <soc/samsung/exynos/imgloader.h>
#endif /* CONFIG_EXYNOS_IMGLOADER */
#if IS_ENABLED(CONFIG_EXYNOS_S2MPU)
#include <soc/samsung/exynos/exynos-s2mpu.h>
#endif /* CONFIG_EXYNOS_S2MPU */
#endif /* CONFIG_DRM_SGPU_EXYNOS */
#include "amdgpu.h"
#include "amdgpu_trace.h"
#include "amdgpu_gfx.h"
#include "nv.h"
#include "nvd.h"

#include "gc/gc_10_4_0_offset.h"
#include "gc/gc_10_4_0_sh_mask.h"
#include "gc/gc_10_4_0_default.h"
#include "smuio/smuio_11_0_0_offset.h"
#include "smuio/smuio_11_0_0_sh_mask.h"
#include "navi10_enum.h"
#include "hdp/hdp_5_0_0_offset.h"
#include "ivsrcid/gfx/irqsrcs_gfx_10_1.h"

#include "soc15.h"
#include "soc15d.h"
#include "soc15_common.h"
#include "clearstate_gfx10.h"
#include "v10_structs.h"
#include "gfx_v10_0.h"
#include "nbio_v2_3.h"
#include "amdgpu_cwsr.h"
#include "amdgpu_tmz.h"

#include "sgpu.h"
#include "sgpu_bpmd.h"
#include "sgpu_bpmd_layout.h"
#include "sgpu_utilization.h"
#include "vangogh_lite_hw_counter.h"
#include "sgpu_debug.h"
#include "sgpu_bpmd_log.h"

/* SGPU FW offsets */
#define CL_VERSION_OFFSET   0xd8038
#define SGPU_VERSION_OFFSET 0xd8000
#define FW_SIGN_SIZE        528
#define RLC_VERSION_OFFSET  0x18
#define RLC_SIZE_OFFSET     0x40
#define PFP_VERSION_OFFSET  0x20
#define PFP_SIZE_OFFSET     0x48
#define ME_VERSION_OFFSET   0x28
#define ME_SIZE_OFFSET      0x50
#define MEC_VERSION_OFFSET  0x30
#define MEC_SIZE_OFFSET     0x58
#define RTL_CLNUM_OFFSET    0x38
#define SIGNATURE_OFFSET    0x3df0
#define GET_U32_OFFSET(offset) (offset/sizeof(u32))

/**
 * Navi10 has two graphic rings to share each graphic pipe.
 * 1. Primary ring
 * 2. Async ring
 */
#define GFX10_NUM_GFX_RINGS_NV1X	1
#define GFX10_NUM_GFX_RINGS_Sienna_Cichlid	1
#define GFX10_MEC_HPD_SIZE	2048

#define F32_CE_PROGRAM_RAM_SIZE		65536
#define RLCG_UCODE_LOADING_START_ADDRESS	0x00002000L

#define mmCGTT_GS_NGG_CLK_CTRL	0x5087
#define mmCGTT_GS_NGG_CLK_CTRL_BASE_IDX	1
#define mmCGTT_SPI_RA0_CLK_CTRL 0x507a
#define mmCGTT_SPI_RA0_CLK_CTRL_BASE_IDX 1
#define mmCGTT_SPI_RA1_CLK_CTRL 0x507b
#define mmCGTT_SPI_RA1_CLK_CTRL_BASE_IDX 1

#define GB_ADDR_CONFIG__NUM_PKRS__SHIFT                                                                       0x8
#define GB_ADDR_CONFIG__NUM_PKRS_MASK                                                                         0x00000700L

#define mmCP_MEC_CNTL_Vgh_Lite                      0x0f55
#define mmCP_MEC_CNTL_Vgh_Lite_BASE_IDX             0

#define mmCP_MEC_CNTL_Sienna_Cichlid                      0x0f55
#define mmCP_MEC_CNTL_Sienna_Cichlid_BASE_IDX             0
#define mmRLC_SAFE_MODE_Sienna_Cichlid			0x4ca0
#define mmRLC_SAFE_MODE_Sienna_Cichlid_BASE_IDX		1
#define mmRLC_CP_SCHEDULERS_Sienna_Cichlid		0x4ca1
#define mmRLC_CP_SCHEDULERS_Sienna_Cichlid_BASE_IDX	1
#define mmSPI_CONFIG_CNTL_Sienna_Cichlid			0x11ec
#define mmSPI_CONFIG_CNTL_Sienna_Cichlid_BASE_IDX		0
#define mmVGT_ESGS_RING_SIZE_Sienna_Cichlid		0x0fc1
#define mmVGT_ESGS_RING_SIZE_Sienna_Cichlid_BASE_IDX	0
#define mmVGT_GSVS_RING_SIZE_Sienna_Cichlid		0x0fc2
#define mmVGT_GSVS_RING_SIZE_Sienna_Cichlid_BASE_IDX	0
#define mmVGT_TF_RING_SIZE_Sienna_Cichlid			0x0fc3
#define mmVGT_TF_RING_SIZE_Sienna_Cichlid_BASE_IDX	0
#define mmVGT_HS_OFFCHIP_PARAM_Sienna_Cichlid		0x0fc4
#define mmVGT_HS_OFFCHIP_PARAM_Sienna_Cichlid_BASE_IDX	0
#define mmVGT_TF_MEMORY_BASE_Sienna_Cichlid		0x0fc5
#define mmVGT_TF_MEMORY_BASE_Sienna_Cichlid_BASE_IDX	0
#define mmVGT_TF_MEMORY_BASE_HI_Sienna_Cichlid		0x0fc6
#define mmVGT_TF_MEMORY_BASE_HI_Sienna_Cichlid_BASE_IDX	0
#define GRBM_STATUS2__RLC_BUSY_Sienna_Cichlid__SHIFT	0x1a
#define GRBM_STATUS2__RLC_BUSY_Sienna_Cichlid_MASK	0x04000000L
#define CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER_Sienna_Cichlid_MASK	0x00000FFCL
#define CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER_Sienna_Cichlid__SHIFT	0x2
#define CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_Sienna_Cichlid_MASK	0x00000FFCL
#define mmGCR_GENERAL_CNTL_Sienna_Cichlid			0x1580
#define mmGCR_GENERAL_CNTL_Sienna_Cichlid_BASE_IDX	0

#define mmCP_HYP_PFP_UCODE_ADDR			0x5814
#define mmCP_HYP_PFP_UCODE_ADDR_BASE_IDX	1
#define mmCP_HYP_PFP_UCODE_DATA			0x5815
#define mmCP_HYP_PFP_UCODE_DATA_BASE_IDX	1
#define mmCP_HYP_CE_UCODE_ADDR			0x5818
#define mmCP_HYP_CE_UCODE_ADDR_BASE_IDX		1
#define mmCP_HYP_CE_UCODE_DATA			0x5819
#define mmCP_HYP_CE_UCODE_DATA_BASE_IDX		1
#define mmCP_HYP_ME_UCODE_ADDR			0x5816
#define mmCP_HYP_ME_UCODE_ADDR_BASE_IDX		1
#define mmCP_HYP_ME_UCODE_DATA			0x5817
#define mmCP_HYP_ME_UCODE_DATA_BASE_IDX		1

//CC_GC_SA_UNIT_DISABLE
#define mmCC_GC_SA_UNIT_DISABLE                 0x0fe9
#define mmCC_GC_SA_UNIT_DISABLE_BASE_IDX        0
#define CC_GC_SA_UNIT_DISABLE__SA_DISABLE__SHIFT	0x8
#define CC_GC_SA_UNIT_DISABLE__SA_DISABLE_MASK		0x0000FF00L
//GC_USER_SA_UNIT_DISABLE
#define mmGC_USER_SA_UNIT_DISABLE               0x0fea
#define mmGC_USER_SA_UNIT_DISABLE_BASE_IDX      0
#define GC_USER_SA_UNIT_DISABLE__SA_DISABLE__SHIFT	0x8
#define GC_USER_SA_UNIT_DISABLE__SA_DISABLE_MASK	0x0000FF00L
//PA_SC_ENHANCE_3
#define mmPA_SC_ENHANCE_3                       0x1085
#define mmPA_SC_ENHANCE_3_BASE_IDX              0
#define PA_SC_ENHANCE_3__FORCE_PBB_WORKLOAD_MODE_TO_ZERO__SHIFT 0x3
#define PA_SC_ENHANCE_3__FORCE_PBB_WORKLOAD_MODE_TO_ZERO_MASK   0x00000008L

#define mmCGTT_SPI_CS_CLK_CTRL			0x507c
#define mmCGTT_SPI_CS_CLK_CTRL_BASE_IDX         1

#define GB_ADDR_CONFIG_VAL     0x142

#define mmMP1_SMN_EXT_SCRATCH0			0x03c0
#define mmMP1_SMN_EXT_SCRATCH0_BASE_IDX		0
#define MP1_SMN_EXT_SCRATCH0__PWR_GFXOFF_STATUS__SHIFT	0x0
#define MP1_SMN_EXT_SCRATCH0__PWR_GFXOFF_STATUS_MASK	0x00000003l

#define mmM0_SQ_SHADER_TBA_LO    0x10b2
#define mmM0_SQ_SHADER_TBA_LO_BASE_IDX  0
#define mmM0_SQ_SHADER_TBA_HI    0x10b3
#define mmM0_SQ_SHADER_TBA_HI_BASE_IDX  0

#define mmCP_HQD_HQ_STATUS0      0x1fc9
#define mmCP_HQD_HQ_STATUS0_BASE_IDX    0
#define mmCP_HQD_HQ_CONTROL0     0x1fca
#define mmCP_HQD_HQ_CONTROL0_BASE_IDX   0

#define  CP_PFP_HEADER_DUMP_reg_read_count 8
#define  CP_ME_HEADER_DUMP_reg_read_count 8

MODULE_FIRMWARE("amdgpu/navi10_ce.bin");
MODULE_FIRMWARE("amdgpu/navi10_pfp.bin");
MODULE_FIRMWARE("amdgpu/navi10_me.bin");
MODULE_FIRMWARE("amdgpu/navi10_mec.bin");
MODULE_FIRMWARE("amdgpu/navi10_mec2.bin");
MODULE_FIRMWARE("amdgpu/navi10_rlc.bin");

MODULE_FIRMWARE("amdgpu/navi14_ce_wks.bin");
MODULE_FIRMWARE("amdgpu/navi14_pfp_wks.bin");
MODULE_FIRMWARE("amdgpu/navi14_me_wks.bin");
MODULE_FIRMWARE("amdgpu/navi14_mec_wks.bin");
MODULE_FIRMWARE("amdgpu/navi14_mec2_wks.bin");
MODULE_FIRMWARE("amdgpu/navi14_ce.bin");
MODULE_FIRMWARE("amdgpu/navi14_pfp.bin");
MODULE_FIRMWARE("amdgpu/navi14_me.bin");
MODULE_FIRMWARE("amdgpu/navi14_mec.bin");
MODULE_FIRMWARE("amdgpu/navi14_mec2.bin");
MODULE_FIRMWARE("amdgpu/navi14_rlc.bin");

MODULE_FIRMWARE("amdgpu/navi12_ce.bin");
MODULE_FIRMWARE("amdgpu/navi12_pfp.bin");
MODULE_FIRMWARE("amdgpu/navi12_me.bin");
MODULE_FIRMWARE("amdgpu/navi12_mec.bin");
MODULE_FIRMWARE("amdgpu/navi12_mec2.bin");
MODULE_FIRMWARE("amdgpu/navi12_rlc.bin");

MODULE_FIRMWARE("amdgpu/sienna_cichlid_ce.bin");
MODULE_FIRMWARE("amdgpu/sienna_cichlid_pfp.bin");
MODULE_FIRMWARE("amdgpu/sienna_cichlid_me.bin");
MODULE_FIRMWARE("amdgpu/sienna_cichlid_mec.bin");
MODULE_FIRMWARE("amdgpu/sienna_cichlid_mec2.bin");
MODULE_FIRMWARE("amdgpu/sienna_cichlid_rlc.bin");

MODULE_FIRMWARE("amdgpu/navy_flounder_ce.bin");
MODULE_FIRMWARE("amdgpu/navy_flounder_pfp.bin");
MODULE_FIRMWARE("amdgpu/navy_flounder_me.bin");
MODULE_FIRMWARE("amdgpu/navy_flounder_mec.bin");
MODULE_FIRMWARE("amdgpu/navy_flounder_mec2.bin");
MODULE_FIRMWARE("amdgpu/navy_flounder_rlc.bin");

/*
 * amdgpu_device can be dma_coherent and dma_mapping does not maintain caches if the given
 * device is coherent. So we need to have a device descriptor that is noting but for dma-mapping.
 */
static void sgpu_firmware_clean(struct amdgpu_device *adev, off_t offset, size_t len)
{
	dma_sync_single_for_device(&sync_dev,
				   adev->gfx.rlc.rlc_autoload_gpu_addr + offset, len,
				   DMA_TO_DEVICE);
}

#ifdef CONFIG_DRM_SGPU_BUILTIN_FIRMWARE
#include "unified_firmware_sign_m0_evt1.h"
#include "unified_firmware_sign_m1_evt0.h"
#include "unified_firmware_sign_m2_evt0.h"
#include "unified_firmware_sign_mid_evt0.h"
/*
 * For now, Model ID and EVT number(major version) in GRBM_CHI_REVISION is not required to select
 * a firmware for emulators. If they are required to select the proper firmware, then this macro
 * and the table of firmwares should be changed accordingly.
 */
#define MGFX_CHIP_REVISION(gen) (((gen) << MGFX_GEN_FIELD_SHIFT) & MGFX_GEN_MASK)
#define MGFX_M0		0
#define MGFX_M1		1
#define MGFX_M2		2
static const struct {
	uint32_t chip_rev;
	uint32_t *fw_bin;
	size_t fw_size;
} builtin_firmwares[] = {
	{
		MGFX_CHIP_REVISION(MGFX_M0),
		unified_firmware_m0_evt1,
		sizeof(unified_firmware_m0_evt1)
	}, {
		/* TITAN GPU is having the same GEN id but, here it is Viking*/
		MGFX_CHIP_REVISION(MGFX_M1),
		unified_firmware_m1_evt0,
		sizeof(unified_firmware_m1_evt0)
	}, {
		MGFX_CHIP_REVISION(MGFX_M2),
		unified_firmware_m2_evt0,
		sizeof(unified_firmware_m2_evt0)
	}
};

static int copy_builtin_firmware(struct amdgpu_device *adev, uint32_t *fw_bin, size_t fw_size)
{
	struct firmware *fw;

	if (fw_size > adev->gfx.rlc.autoload_size) {
		DRM_ERROR("Too small to load firmware (%zu > %u)\n",
			  fw_size, adev->gfx.rlc.autoload_size);
		return -EINVAL;
	}

	memcpy(adev->gfx.rlc.rlc_autoload_ptr, fw_bin, fw_size);

	fw = kmalloc(sizeof(*fw), GFP_KERNEL);
	if (!fw)
		return -ENOMEM;

	fw->data = adev->gfx.rlc.rlc_autoload_ptr;
	fw->size = fw_size;

	adev->gfx.unified_fw = fw;

	sgpu_firmware_clean(adev, 0, fw_size);

	return 0;
}

static int sgpu_request_firmware(struct amdgpu_device *adev)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(builtin_firmwares); i++)
		if (((adev->grbm_chip_rev) & MGFX_GEN_MASK) == builtin_firmwares[i].chip_rev)
			return copy_builtin_firmware(adev, builtin_firmwares[i].fw_bin,
						       builtin_firmwares[i].fw_size);

	return -EINVAL;
}

#else /* !CONFIG_DRM_SGPU_BUILTIN_FIRMWARE */
MODULE_FIRMWARE("sgpu/vangogh_lite_unified_evt0.bin");
MODULE_FIRMWARE("sgpu/vangogh_lite_unified_evt1.bin");

#ifdef CONFIG_DRM_SGPU_EXYNOS
/*
 * GRBM_CHIP_REVISION is not accessible before GPU booting.
 * So let's use CHIPID instead in real silicon
 */
static unsigned int sgpu_chip_revision(uint32_t grbm_chip_rev)
{
	return  !exynos_soc_info.revision ? exynos_soc_info.revision : exynos_soc_info.main_rev;
}
#else
static unsigned int sgpu_chip_revision(uint32_t grbm_chip_rev)
{
	return MGFX_EVT(grbm_chip_rev) - 1; /* MGFX_EVT() returns 1 for EVT0. */
}
#endif
static const char *firmware_name(uint32_t grbm_chip_rev)
{
	static char firmware_name_template[] = "sgpu/vangogh_lite_unified_evt0.bin";

	scnprintf(firmware_name_template, sizeof(firmware_name_template),
		  "sgpu/vangogh_lite_unified_evt%d.bin", sgpu_chip_revision(grbm_chip_rev));
	return firmware_name_template;
}

#if IS_ENABLED(CONFIG_EXYNOS_IMGLOADER)
#if IS_ENABLED(CONFIG_EXYNOS_S2MPU)
static int g3d_imgloader_verify_fw(struct imgloader_desc *desc,
				   phys_addr_t fw_phys_base,
				   size_t fw_bin_size, size_t fw_mem_size)
{
	uint64_t ret64;

	/* Request F/W verification. This must be requested after power on */
	ret64 = exynos_verify_subsystem_fw(desc->name, desc->fw_id,
					   fw_phys_base, fw_bin_size,
					   fw_mem_size);
	if (ret64) {
		DRM_ERROR("Failed F/W verification, ret=%llu\n", ret64);
		return -EIO;
	}
	return 0;
}
#else /* !CONFIG_EXYNOS_S2MPU */
static int g3d_imgloader_verify_fw(struct imgloader_desc *desc,
				   phys_addr_t fw_phys_base,
				   size_t fw_bin_size, size_t fw_mem_size)
{
	/* TODO: use named literal for 0x82000520 */
	unsigned long ret;

	ret = exynos_smc(0x82000520, 0x10000, fw_phys_base, fw_mem_size);
	if (ret) {
		DRM_ERROR("Failed load F/W from smc(0x82000520), ret=%#lx\n", ret);
		return -EIO;
	}

	return 0;
}
#endif /* CONFIG_EXYNOS_S2MPU */
static int g3d_imgloader_mem_setup(struct imgloader_desc *desc, const u8 *fw_data,
			    size_t fw_size, phys_addr_t *fw_phys_base,
			    size_t *fw_bin_size, size_t *fw_mem_size)
{
	struct amdgpu_device *adev = drm_to_adev(dev_get_drvdata(desc->dev));
	int r = 0;

	r = request_firmware_into_buf(&adev->gfx.unified_fw,
				      desc->fw_name,
				      adev->dev,
				      adev->gfx.rlc.rlc_autoload_ptr,
				      adev->gfx.rlc.autoload_size);
	if (r) {
		DRM_ERROR("failed request firmware\n");
		return r;
	}

	*fw_phys_base = adev->gfx.rlc.rlc_autoload_gpu_addr;
	*fw_bin_size = adev->gfx.unified_fw->size;
	*fw_mem_size = adev->gfx.rlc.autoload_size;

	sgpu_firmware_clean(adev, 0, adev->gfx.unified_fw->size);

	return 0;
}

struct imgloader_ops g3d_imgloader_ops = {
	.mem_setup = g3d_imgloader_mem_setup,
	.verify_fw = g3d_imgloader_verify_fw,
	.blk_pwron = NULL,
	.deinit_image = NULL,
};

static int sgpu_request_firmware(struct amdgpu_device *adev)
{
	struct imgloader_desc desc;

	desc.dev = &adev->pldev->dev;
	desc.owner = THIS_MODULE;
	desc.ops = &g3d_imgloader_ops;
	desc.fw_name = firmware_name(adev->grbm_chip_rev);
	desc.name = "G3D_TMR";
	desc.s2mpu_support = false;
	desc.fw_id = 0;
	desc.skip_request_firmware = true;

	imgloader_desc_init(&desc);

	return imgloader_boot(&desc);
}
#else /* !CONFIG_EXYNOS_IMGLOADER */
#if IS_ENABLED(CONFIG_EXYNOS_S2MPU)
static int sgpu_request_firmware(struct amdgpu_device *adev)
{
	int ret;

	ret = request_firmware_into_buf(&adev->gfx.unified_fw, firmware_name(adev->grbm_chip_rev),
					adev->dev,
					adev->gfx.rlc.rlc_autoload_ptr,
					adev->gfx.rlc.autoload_size);
	if (ret)
		return ret;

	sgpu_firmware_clean(adev, 0, adev->gfx.unified_fw->size);

	exynos_s2mpu_verify_subsystem_fw("G3D_TMR",
					0,
					adev->gfx.rlc.rlc_autoload_gpu_addr,
					adev->gfx.unified_fw->size,
					adev->gfx.rlc.autoload_size);

	exynos_s2mpu_request_fw_stage2_ap("G3D_TMR");

	return 0;
}
#else /* !CONFIG_EXYNOS_S2MPU */
static int sgpu_request_firmware(struct amdgpu_device *adev)
{
	int ret;

	ret = request_firmware_into_buf(&adev->gfx.unified_fw, firmware_name(adev->grbm_chip_rev),
					adev->dev,
					adev->gfx.rlc.rlc_autoload_ptr,
					adev->gfx.rlc.autoload_size);
	if (ret)
		return ret;

	sgpu_firmware_clean(adev, 0, adev->gfx.unified_fw->size);

	ret = exynos_smc(0x82000520, 0x10000,
			adev->gfx.rlc.rlc_autoload_gpu_addr,
			adev->gfx.rlc.autoload_size);
	if (ret) {
		DRM_ERROR("Failed to notify firmware info, ret=%#x\n", ret);
		return -EIO;
	}

	return 0;
}
#endif /* CONFIG_EXYNOS_S2MPU */
#endif /* CONFIG_EXYNOS_IMGLOADER */
#endif /* CONFIG_DRM_SGPU_BUILTIN_FIRMWARE */

static struct gc_down_config gc_cfg_tb[] = {
	{
		eval_mode_config_disabled, // config_mode
		false,                        // change_adapter_info
		0,                            // wgps_per_sa
		0,                            // rbs_per_sa
		0,                            // se_count
		0,                            // sas_per_se
		0,                            // packers_per_sc
		0,                            // num_scs
		false,                        // write_shader_array_config
		false,                        // use_reset_lowest_vgt_fw
		true,                         // force_gl1_miss
	},
	{
		eval_mode_config_gc_10_4, // config_mode M0
		true,                     // change_adapter_info
		3,                        // wgps_per_sa
		3,                        // rbs_per_sa
		1,                        // se_count
		1,                        // sas_per_se
		2,                        // packers_per_sc
		1,                        // num_scs
		true,                     // write_shader_array_config
		true,                     // use_reset_lowest_vgt_fw
		false,                    // force_gl1_miss
	},
	{
		eval_mode_config_gc_40_1, // config_mode M1
		true,                     // change_adapter_info
		4,                        // wgps_per_sa
		3,                        // rbs_per_sa
		1,                        // se_count
		1,                        // sas_per_se
		2,                        // packers_per_sc
		1,                        // num_scs
		true,                     // write_shader_array_config
		true,                     // use_reset_lowest_vgt_fw
		false,                    // force_gl1_miss
	},
	{
		eval_mode_config_gc_40_2, // config_mode M2
		true,                     // change_adapter_info
		3,                        // wgps_per_sa
		2,                        // rbs_per_sa
		1,                        // se_count
		2,                        // sas_per_se
		4,                        // packers_per_sc
		1,                        // num_scs
		true,                     // write_shader_array_config
		true,                     // use_reset_lowest_vgt_fw
		false,                    // force_gl1_miss
	},
};

static const struct soc15_reg_golden golden_settings_gc_10_1[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCB_HW_CONTROL_4, 0xffffffff, 0x00400014),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_CPF_CLK_CTRL, 0xfcff8fff, 0xf8000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SPI_CLK_CTRL, 0xcd000000, 0x0d000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SQ_CLK_CTRL, 0x60000ff0, 0x60000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SQG_CLK_CTRL, 0x40000000, 0x40000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_VGT_CLK_CTRL, 0xffff8fff, 0xffff8100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_WD_CLK_CTRL, 0xfeff8fff, 0xfeff8100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCH_PIPE_STEER, 0xffffffff, 0xe4e4e4e4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCH_VC5_ENABLE, 0x00000002, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCP_SD_CNTL, 0x000007ff, 0x000005ff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG, 0x20000000, 0x20000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG2, 0xffffffff, 0x00000420),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG3, 0x00000200, 0x00000200),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG4, 0x07900000, 0x04900000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DFSM_TILES_IN_FLIGHT, 0x0000ffff, 0x0000003f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_LAST_OF_BURST_CONFIG, 0xffffffff, 0x03860204),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGCR_GENERAL_CNTL, 0x1ff0ffff, 0x00000500),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGE_PRIV_CONTROL, 0x000007ff, 0x000001fe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL1_PIPE_STEER, 0xffffffff, 0xe4e4e4e4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2_PIPE_STEER_0, 0x77777777, 0x10321032),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2_PIPE_STEER_1, 0x77777777, 0x02310231),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2A_ADDR_MATCH_MASK, 0xffffffff, 0xffffffcf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_ADDR_MATCH_MASK, 0xffffffff, 0xffffffcf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CGTT_SCLK_CTRL, 0x10000000, 0x10000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CTRL2, 0xffffffff, 0x1402002f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CTRL3, 0xffff9fff, 0x00001188),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_BINNER_TIMEOUT_COUNTER, 0xffffffff, 0x00000800),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE, 0x3fffffff, 0x08000009),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE_1, 0x00400000, 0x04440000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE_2, 0x00000800, 0x00000820),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_LINE_STIPPLE_STATE, 0x0000ff0f, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRMI_SPARE, 0xffffffff, 0xffff3101),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_CONFIG_CNTL_1, 0x001f0000, 0x00070104),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_ALU_CLK_CTRL, 0xffffffff, 0xffffffff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_ARB_CONFIG, 0x00000100, 0x00000130),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_LDS_CLK_CTRL, 0xffffffff, 0xffffffff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTA_CNTL_AUX, 0xfff7ffff, 0x01030000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTCP_CNTL, 0x60000010, 0x479c0010),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmUTCL1_CGTT_CLK_CTRL, 0xfeff0fff, 0x40000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmUTCL1_CTRL, 0x00c00000, 0x00c00000)
};

static int gfx_v10_0_compute_mqd_init(struct amdgpu_ring *ring);
static int gfx_v10_0_kiq_init_register(struct amdgpu_ring *ring);
static int gfx_v10_0_pio_map_queue(struct amdgpu_ring *ring);
static int gfx_v10_0_pio_unmap_queue(struct amdgpu_ring *ring,
				      u32 dequeue_type);
static u64 gfx_v10_0_ring_get_wptr_compute(struct amdgpu_ring *ring);

static const struct soc15_reg_golden golden_settings_gc_10_0_nv10[] =
{
	/* Pending on emulation bring up */
};

static const struct soc15_reg_golden golden_settings_gc_rlc_spm_10_0_nv10[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xe0000000, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x28),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xcc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x88),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xbc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x90),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x94),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x98),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x9c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xac),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x38),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x3c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x40),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x44),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x48),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x70),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x74),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x78),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x7c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x80),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x84),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x50),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x54),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x58),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x5c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x60),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x64),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x68),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x6c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x28),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x28),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x2c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x2c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x30),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x30),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x34),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x34),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x38),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x38),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x3c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x3c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x50),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x50),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x54),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x54),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x58),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x58),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x5c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x5c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1d),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1d),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x48),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x48),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x40),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x40),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x44),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x44),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x17),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x17),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x60),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x60),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x64),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x64),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x70),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x70),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x74),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x74),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x68),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x68),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x6c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x6c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x78),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x78),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x7c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x7c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x88),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x88),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x80),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x80),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x84),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x84),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x90),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x90),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x94),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x94),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x98),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x98),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x9c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x9c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xac),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xac),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xbc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xbc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xcc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xcc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xec),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xec),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xfc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xfc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x104),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x104),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x118),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x118),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x11c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x11c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x120),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x120),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x124),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x124),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xdc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xdc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x110),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x110),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x114),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x114),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x108),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x108),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x17),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x17),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x128),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x128),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x12c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x12c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x138),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x138),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x13c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x13c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x130),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x130),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x134),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x134),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x140),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x140),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x144),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x144),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x150),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x150),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x154),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x154),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x148),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x148),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x158),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x158),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x15c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x15c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x168),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x168),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x16c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x16c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x160),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x160),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x164),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x164),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x170),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x170),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x174),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x174),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x180),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x180),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x184),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x184),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x178),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x178),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x17c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x17c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x188),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x188),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x198),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x198),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x19c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x19c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x190),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x190),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x194),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x194),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x30),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x34),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1d),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1d),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x2c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLOBALS_SAMPLE_SKEW, 0x000000FF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLOBALS_MUXSEL_SKEW, 0x000000FF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLE_SKEW, 0x000000FF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLE_SKEW, 0x000000FF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_MUXSEL_SKEW, 0x000000FF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_MUXSEL_SKEW, 0x000000FF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_DESER_START_SKEW, 0x000000FF, 0x33),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xFFFFFFFF, 0xe0000000)
};

static const struct soc15_reg_golden golden_settings_gc_10_1_1[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCB_HW_CONTROL_4, 0xffffffff, 0x003c0014),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_GS_NGG_CLK_CTRL, 0xffff8fff, 0xffff8100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_IA_CLK_CTRL, 0xffff0fff, 0xffff0100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SPI_CLK_CTRL, 0xcd000000, 0x0d000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SQ_CLK_CTRL, 0xf8ff0fff, 0x60000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SQG_CLK_CTRL, 0x40000ff0, 0x40000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_VGT_CLK_CTRL, 0xffff8fff, 0xffff8100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_WD_CLK_CTRL, 0xffff8fff, 0xffff8100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCH_PIPE_STEER, 0xffffffff, 0xe4e4e4e4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCH_VC5_ENABLE, 0x00000002, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCP_SD_CNTL, 0x800007ff, 0x000005ff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG, 0xffffffff, 0x20000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG2, 0xffffffff, 0x00000420),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG3, 0x00000200, 0x00000200),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG4, 0xffffffff, 0x04900000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DFSM_TILES_IN_FLIGHT, 0x0000ffff, 0x0000003f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_LAST_OF_BURST_CONFIG, 0xffffffff, 0x03860204),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGCR_GENERAL_CNTL, 0x1ff0ffff, 0x00000500),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGE_PRIV_CONTROL, 0x000007ff, 0x000001fe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL1_PIPE_STEER, 0xffffffff, 0xe4e4e4e4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2A_ADDR_MATCH_MASK, 0xffffffff, 0xffffffe7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_ADDR_MATCH_MASK, 0xffffffff, 0xffffffe7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CGTT_SCLK_CTRL, 0xffff0fff, 0x10000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CTRL2, 0xffffffff, 0x1402002f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CTRL3, 0xffffbfff, 0x00000188),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_BINNER_TIMEOUT_COUNTER, 0xffffffff, 0x00000800),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE, 0x3fffffff, 0x08000009),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE_1, 0x00400000, 0x04440000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE_2, 0x00000800, 0x00000820),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_LINE_STIPPLE_STATE, 0x0000ff0f, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRMI_SPARE, 0xffffffff, 0xffff3101),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_CONFIG_CNTL_1, 0x001f0000, 0x00070105),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_ALU_CLK_CTRL, 0xffffffff, 0xffffffff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_ARB_CONFIG, 0x00000133, 0x00000130),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_LDS_CLK_CTRL, 0xffffffff, 0xffffffff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTA_CNTL_AUX, 0xfff7ffff, 0x01030000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTCP_CNTL, 0x60000010, 0x479c0010),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmUTCL1_CTRL, 0x00c00000, 0x00c00000),
};

static const struct soc15_reg_golden golden_settings_gc_10_1_2[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCB_HW_CONTROL_4, 0x003e001f, 0x003c0014),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_GS_NGG_CLK_CTRL, 0xffff8fff, 0xffff8100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_IA_CLK_CTRL, 0xffff0fff, 0xffff0100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SPI_CLK_CTRL, 0xff7f0fff, 0x0d000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SQ_CLK_CTRL, 0xffffcfff, 0x60000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SQG_CLK_CTRL, 0xffff0fff, 0x40000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_VGT_CLK_CTRL, 0xffff8fff, 0xffff8100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_WD_CLK_CTRL, 0xffff8fff, 0xffff8100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCH_PIPE_STEER, 0xffffffff, 0xe4e4e4e4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCH_VC5_ENABLE, 0x00000003, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCP_SD_CNTL, 0x800007ff, 0x000005ff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG, 0xffffffff, 0x20000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG2, 0xffffffff, 0x00000420),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG3, 0xffffffff, 0x00000200),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG4, 0xffffffff, 0x04800000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DFSM_TILES_IN_FLIGHT, 0x0000ffff, 0x0000003f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_LAST_OF_BURST_CONFIG, 0xffffffff, 0x03860204),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGCR_GENERAL_CNTL, 0x1ff0ffff, 0x00000500),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGE_PRIV_CONTROL, 0x00007fff, 0x000001fe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL1_PIPE_STEER, 0xffffffff, 0xe4e4e4e4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2_PIPE_STEER_0, 0x77777777, 0x10321032),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2_PIPE_STEER_1, 0x77777777, 0x02310231),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2A_ADDR_MATCH_MASK, 0xffffffff, 0xffffffcf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_ADDR_MATCH_MASK, 0xffffffff, 0xffffffcf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CGTT_SCLK_CTRL, 0xffff0fff, 0x10000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CTRL2, 0xffffffff, 0x1402002f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CTRL3, 0xffffbfff, 0x00000188),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_BINNER_EVENT_CNTL_0, 0xffffffff, 0x842a4c02),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_BINNER_TIMEOUT_COUNTER, 0xffffffff, 0x00000800),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE, 0x3fffffff, 0x08000009),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE_1, 0xffffffff, 0x04440000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE_2, 0x00000820, 0x00000820),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_LINE_STIPPLE_STATE, 0x0000ff0f, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRMI_SPARE, 0xffffffff, 0xffff3101),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_ALU_CLK_CTRL, 0xffffffff, 0xffffffff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_ARB_CONFIG, 0x00000133, 0x00000130),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_LDS_CLK_CTRL, 0xffffffff, 0xffffffff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTA_CNTL_AUX, 0xfff7ffff, 0x01030000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTCP_CNTL, 0xffdf80ff, 0x479c0010),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmUTCL1_CTRL, 0xffffffff, 0x00800000)
};

static void gfx_v10_rlcg_wreg(struct amdgpu_device *adev, u32 offset, u32 v)
{
	static void *scratch_reg0;
	static void *scratch_reg1;
	static void *scratch_reg2;
	static void *scratch_reg3;
	static void *spare_int;
	static uint32_t grbm_cntl;
	static uint32_t grbm_idx;
	uint32_t i = 0;
	uint32_t retries = 50000;

	scratch_reg0 = adev->rmmio + (adev->reg_offset[GC_HWIP][0][mmSCRATCH_REG0_BASE_IDX] + mmSCRATCH_REG0)*4;
	scratch_reg1 = adev->rmmio + (adev->reg_offset[GC_HWIP][0][mmSCRATCH_REG1_BASE_IDX] + mmSCRATCH_REG1)*4;
	scratch_reg2 = adev->rmmio + (adev->reg_offset[GC_HWIP][0][mmSCRATCH_REG1_BASE_IDX] + mmSCRATCH_REG2)*4;
	scratch_reg3 = adev->rmmio + (adev->reg_offset[GC_HWIP][0][mmSCRATCH_REG1_BASE_IDX] + mmSCRATCH_REG3)*4;
	spare_int = adev->rmmio + (adev->reg_offset[GC_HWIP][0][mmRLC_SPARE_INT_BASE_IDX] + mmRLC_SPARE_INT)*4;

	grbm_cntl = adev->reg_offset[GC_HWIP][0][mmGRBM_GFX_CNTL_BASE_IDX] + mmGRBM_GFX_CNTL;
	grbm_idx = adev->reg_offset[GC_HWIP][0][mmGRBM_GFX_INDEX_BASE_IDX] + mmGRBM_GFX_INDEX;

	if (amdgpu_sriov_runtime(adev)) {
		pr_err("shouldn't call rlcg write register during runtime\n");
		return;
	}

	writel(v, scratch_reg0);
	writel(offset | 0x80000000, scratch_reg1);
	writel(1, spare_int);
	for (i = 0; i < retries; i++) {
		u32 tmp;

		tmp = readl(scratch_reg1);
		if (!(tmp & 0x80000000))
			break;

		udelay(10);
	}

	if (i >= retries)
		pr_err("timeout: rlcg program reg:0x%05x failed !\n", offset);
}

static const struct soc15_reg_golden golden_settings_gc_10_1_nv14[] =
{
	/* Pending on emulation bring up */
};

static const struct soc15_reg_golden golden_settings_gc_rlc_spm_10_1_nv14[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xE0000000L, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x28),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x80),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x84),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x88),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x60),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x64),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x68),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x6c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x70),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x74),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x78),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x7c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x38),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x3c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x40),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x44),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x48),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x50),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x54),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x58),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x5c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x28),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x2c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x30),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x34),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x38),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x3c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x50),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1e),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x54),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1e),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x58),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1e),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x5c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1e),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x48),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x40),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1e),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x44),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1e),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x60),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x64),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x70),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x74),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x68),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x6c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x78),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x7c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x88),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x80),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x84),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x90),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x94),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x98),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x9c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xac),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xbc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xcc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xdc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xec),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x104),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x108),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x110),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x114),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x118),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x11c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x17),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x130),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x134),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x138),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x13c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x128),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x12c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x120),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x124),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x140),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x144),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x150),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x154),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x148),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x158),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x15c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x168),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x16c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x160),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x164),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x170),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x174),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x180),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x184),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x178),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x17c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x188),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x198),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x19c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x190),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x194),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1a0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1a4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1b0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1b4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1a8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1ac),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1b8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1bc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1c8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1cc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1c0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1c4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x30),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x34),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x2c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLOBALS_SAMPLE_SKEW, 0x000000FF, 0x26),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLOBALS_MUXSEL_SKEW, 0x000000FF, 0x28),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLE_SKEW, 0x000000FF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLE_SKEW, 0x000000FF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_MUXSEL_SKEW, 0x000000FF, 0x1f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_MUXSEL_SKEW, 0x000000FF, 0x25),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_DESER_START_SKEW, 0x000000FF, 0x3b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xFFFFFFFF, 0xe0000000)
};

static const struct soc15_reg_golden golden_settings_gc_10_1_2_nv12[] =
{
	/* Pending on emulation bring up */
};

static const struct soc15_reg_golden golden_settings_gc_rlc_spm_10_1_2_nv12[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xe0000000L, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x28),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xcc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x88),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xbc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x90),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x2),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x94),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x98),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x9c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xac),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x38),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x3c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x40),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x44),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x48),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x70),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x74),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x78),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x7c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x80),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x84),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x50),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x54),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x58),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x5c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x60),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x64),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x68),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x6c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x20),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x24),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x28),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x28),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x2c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x2c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x30),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1d),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x30),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1d),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x34),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x34),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x38),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x38),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x3c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x3c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x18),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x50),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x50),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x54),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x54),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x58),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x58),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x5c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x5c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x48),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x48),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x40),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x40),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x44),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x44),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1a),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x60),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x60),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x64),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x64),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x70),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x70),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x74),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x74),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x68),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x68),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x6c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x6c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x78),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x78),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x7c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x7c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x88),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x88),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x8c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x80),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x80),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x84),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x84),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x90),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x90),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x94),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x94),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x98),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x98),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x9c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x9c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xa8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xac),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xac),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xbc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xbc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xb4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xc8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xcc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xcc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xec),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xec),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x16),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xf8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xfc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x17),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xfc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x17),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x13),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x104),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x104),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xe0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x118),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x118),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x11c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x11c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x120),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x120),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x124),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x124),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xdc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xdc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x110),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x110),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x114),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x114),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x14),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x108),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x108),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x10c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x19),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0xd8),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1b),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x128),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x128),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x12c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x12c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x138),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x138),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x13c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x13c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x130),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x130),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x12),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x134),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x134),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x140),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x140),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x144),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x144),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x150),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x150),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x154),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x154),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x148),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x148),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x14c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x7),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x158),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x158),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x15c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x15c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x168),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x168),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xa),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x16c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x16c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x9),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x160),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x160),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x164),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x164),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x170),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x170),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x174),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x174),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x180),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x180),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x184),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x184),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x178),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x178),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x17c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x17c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x188),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x188),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x18c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x5),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x198),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x198),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xc),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x19c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x19c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x190),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x190),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xe),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x194),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x194),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x30),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xd),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x34),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x11),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1d),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1d),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x4),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0x1f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_ADDR, 0xFFFFFFFF, 0x2c),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLB_SAMPLEDELAY_IND_DATA, 0xFFFFFFFF, 0xb),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLOBALS_SAMPLE_SKEW, 0x000000FF, 0x1f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_GLOBALS_MUXSEL_SKEW, 0x000000FF, 0x22),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLE_SKEW, 0x000000FF, 0x1),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_SAMPLE_SKEW, 0x000000FF, 0x6),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_MUXSEL_SKEW, 0x000000FF, 0x10),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x10000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_SE_MUXSEL_SKEW, 0x000000FF, 0x15),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xffffff, 0x0),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRLC_SPM_DESER_START_SKEW, 0x000000FF, 0x35),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_GFX_INDEX, 0xFFFFFFFF, 0xe0000000)
};

static const struct soc15_reg_golden golden_settings_gc_10_3[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SPI_CS_CLK_CTRL, 0x78000000, 0x78000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SPI_PS_CLK_CTRL, 0xff7f0fff, 0x78000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SPI_RA0_CLK_CTRL, 0xff7f0fff, 0x30000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SPI_RA1_CLK_CTRL, 0xff7f0fff, 0x7e000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCPF_GCR_CNTL, 0x0007ffff, 0x0000c000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG3, 0xffffffff, 0x00000280),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG4, 0xffffffff, 0x00800000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_EXCEPTION_CONTROL, 0x7fff0f1f, 0x00b80000),
	SOC15_REG_GOLDEN_VALUE(GC, 0 ,mmGCEA_SDP_TAG_RESERVE0, 0xffffffff, 0x10100100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGCEA_SDP_TAG_RESERVE1, 0xffffffff, 0x17000088),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGCR_GENERAL_CNTL_Sienna_Cichlid, 0x1ff1ffff, 0x00000500),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGE_PC_CNTL, 0x003fffff, 0x00280400),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2A_ADDR_MATCH_MASK, 0xffffffff, 0xffffffcf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_ADDR_MATCH_MASK, 0xffffffff, 0xffffffcf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CM_CTRL1, 0xff8fff0f, 0x580f1008),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CTRL3, 0xf7ffffff, 0x10f80988),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmLDS_CONFIG,  0x00000020, 0x00000020),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_CL_ENHANCE, 0xf17fffff, 0x01200007),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_BINNER_TIMEOUT_COUNTER, 0xffffffff, 0x00000800),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE_2, 0xffffffbf, 0x00000820),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_CONFIG_CNTL_1, 0xffffffff, 0x00070104),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_CONFIG, 0xe07df47f, 0x00180070),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER0_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER1_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER10_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER11_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER12_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER13_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER14_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER15_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER2_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER3_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER4_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER5_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER6_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER7_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER8_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER9_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTA_CNTL_AUX, 0xfff7ffff, 0x01030000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmUTCL1_CTRL, 0xffbfffff, 0x00a00000)
};

static const struct soc15_reg_golden golden_settings_gc_10_3_sienna_cichlid[] =
{
	/* Pending on emulation bring up */
};

static const struct soc15_reg_golden golden_settings_gc_10_3_2[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SPI_PS_CLK_CTRL, 0xff7f0fff, 0x78000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SPI_RA0_CLK_CTRL, 0xff7f0fff, 0x30000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCGTT_SPI_RA1_CLK_CTRL, 0xff7f0fff, 0x7e000100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCPF_GCR_CNTL, 0x0007ffff, 0x0000c000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG3, 0xffffffff, 0x00000280),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG4, 0xffffffff, 0x00800000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_EXCEPTION_CONTROL, 0x7fff0f1f, 0x00b80000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGCR_GENERAL_CNTL_Sienna_Cichlid, 0x1ff1ffff, 0x00000500),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGE_PC_CNTL, 0x003fffff, 0x00280400),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2A_ADDR_MATCH_MASK, 0xffffffff, 0xffffffcf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_ADDR_MATCH_MASK, 0xffffffff, 0xffffffcf),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CM_CTRL1, 0xff8fff0f, 0x580f1008),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_CTRL3, 0xf7ffffff, 0x00f80988),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_CL_ENHANCE, 0xf17fffff, 0x01200007),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_BINNER_TIMEOUT_COUNTER, 0xffffffff, 0x00000800),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE_2, 0xffffffbf, 0x00000820),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_CONFIG_CNTL_1, 0xffffffff, 0x00070104),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_START_PHASE, 0x000000ff, 0x00000004),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_CONFIG, 0xe07df47f, 0x00180070),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER0_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER1_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER10_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER11_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER12_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER13_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER14_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER15_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER2_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER3_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER4_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER5_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER6_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER7_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER8_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQ_PERFCOUNTER9_SELECT, 0xf0f001ff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTA_CNTL_AUX, 0xfff7ffff, 0x01030000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmUTCL1_CTRL, 0xffbfffff, 0x00a00000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmVGT_GS_MAX_WAVE_ID, 0x00000fff, 0x000003ff)
};

/* Based on Voyager Integration Guide */
static const struct soc15_reg_golden golden_settings_gc_10_3_mb0[] = {
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQG_CONFIG,		0x00003fff, 0x2000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2C_ADDR_MATCH_MASK,	0xffffffff, 0xFFFFFFF3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGL2A_ADDR_MATCH_MASK,	0xffffffff, 0xFFFFFFF3),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_CL_ENHANCE,		0xf17fffff, 0x01200007),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGCR_GENERAL_CNTL,	0x0001ffff, 0x00000500),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_CONFIG_CNTL_1,	0xffffffff, 0x800C0100),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmVGT_NUM_INSTANCES,	0x00000000, 0x01),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmVGT_MAX_VTX_INDX,	0x00000000, 0xFFFFFFFF),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_RASTER_CONFIG,	0x00000000, 0x00),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCB_PERFCOUNTER0_SELECT1,0x00000000, 0x00),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG2, 0xffffffff, 0x00000421),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG3, 0x00000200, 0x00000200),
};

#define DEFAULT_SH_MEM_CONFIG \
	((SH_MEM_ADDRESS_MODE_64 << SH_MEM_CONFIG__ADDRESS_MODE__SHIFT) | \
	 (SH_MEM_ALIGNMENT_MODE_UNALIGNED << SH_MEM_CONFIG__ALIGNMENT_MODE__SHIFT) | \
	 (SH_MEM_RETRY_MODE_ALL << SH_MEM_CONFIG__RETRY_MODE__SHIFT) | \
	 (3 << SH_MEM_CONFIG__INITIAL_INST_PREFETCH__SHIFT))


static void gfx_v10_0_set_ring_funcs(struct amdgpu_device *adev);
static void gfx_v10_0_set_irq_funcs(struct amdgpu_device *adev);
static void gfx_v10_0_set_gds_init(struct amdgpu_device *adev);
static void gfx_v10_0_set_rlc_funcs(struct amdgpu_device *adev);
static void gfx_v10_0_set_bpmd_funcs(struct amdgpu_device *adev);
static int gfx_v10_0_get_cu_info(struct amdgpu_device *adev,
                                 struct amdgpu_cu_info *cu_info);
static void gfx_v10_0_static_wgp_clockgating_init(struct amdgpu_device *adev);
static uint64_t gfx_v10_0_get_gpu_clock_counter(struct amdgpu_device *adev);
static u32 gfx_v10_0_select_se_sh(struct amdgpu_device *adev, u32 se_num,
				   u32 sh_num, u32 instance);
static u32 gfx_v10_0_get_wgp_active_bitmap_per_sh(struct amdgpu_device *adev);

static int gfx_v10_0_rlc_backdoor_autoload_buffer_init(struct amdgpu_device *adev);
static void gfx_v10_0_rlc_backdoor_autoload_buffer_fini(struct amdgpu_device *adev);
static int gfx_v10_0_rlc_backdoor_autoload_enable(struct amdgpu_device *adev);
static int gfx_v10_0_wait_for_rlc_autoload_complete(struct amdgpu_device *adev);
static void gfx_v10_0_ring_emit_ce_meta(struct amdgpu_ring *ring, bool resume);
static void gfx_v10_0_ring_emit_de_meta(struct amdgpu_ring *ring, bool resume);
static void gfx_v10_0_ring_emit_frame_cntl(struct amdgpu_ring *ring, bool start, bool secure);
static int gfx_v10_0_bpmd_dump_reg(struct sgpu_bpmd_output *sbo,
				   struct amdgpu_device *adev);
static uint32_t gfx_v10_0_bpmd_dump_ring(struct sgpu_bpmd_output *sbo,
					 struct amdgpu_ring *ring);
static void gfx_v10_0_bpmd_find_ibs(const uint32_t *addr, uint32_t size,
				    uint32_t vmid, struct list_head *list);

static int gfx_v10_0_update_gfx_CGCG(struct amdgpu_device *adev, bool enable);
static int gfx_v10_0_set_powergating_state(void *handle,
					  enum amd_powergating_state state);
static int gfx_v10_0_set_clockgating_state(void *handle,
					  enum amd_clockgating_state state);
static void gfx_v10_0_set_safe_mode(struct amdgpu_device *adev);
static void gfx_v10_0_unset_safe_mode(struct amdgpu_device *adev);
static void gfx_v10_0_update_medium_grain_clock_gating(struct amdgpu_device *adev,
						       bool enable);

static void gfx_v10_0_init_spm_golden_registers(struct amdgpu_device *adev)
{
	switch (adev->asic_type) {
	case CHIP_VANGOGH_LITE:
		adev->gfx.config.max_hw_contexts = 8;
		adev->gfx.config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx.config.sc_prim_fifo_size_backend = 0x100;
		adev->gfx.config.sc_hiz_tile_fifo_size = 0;
		adev->gfx.config.sc_earlyz_tile_fifo_size = 0x4C0;
	break;

	default:
		break;
	}
}

static void gfx_v10_0_init_golden_registers(struct amdgpu_device *adev)
{
	if (adev->asic_type != CHIP_VANGOGH_LITE)
		return;

	soc15_program_register_sequence(adev,
					golden_settings_gc_10_3_mb0,
					(const u32)ARRAY_SIZE(
					golden_settings_gc_10_3_mb0));
	gfx_v10_0_init_spm_golden_registers(adev);
}

static void gfx_v10_0_scratch_init(struct amdgpu_device *adev)
{
	adev->gfx.scratch.num_reg = 8;
	adev->gfx.scratch.reg_base = SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0);
	adev->gfx.scratch.free_mask = (1u << adev->gfx.scratch.num_reg) - 1;
}

static void gfx_v10_0_write_data_to_reg(struct amdgpu_ring *ring, int eng_sel,
				       bool wc, uint32_t reg, uint32_t val)
{
	amdgpu_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
	amdgpu_ring_write(ring, WRITE_DATA_ENGINE_SEL(eng_sel) |
			  WRITE_DATA_DST_SEL(0) | (wc ? WR_CONFIRM : 0));
	amdgpu_ring_write(ring, reg);
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, val);
}

static void gfx_v10_0_wait_reg_mem(struct amdgpu_ring *ring, int eng_sel,
				  int mem_space, int opt, uint32_t addr0,
				  uint32_t addr1, uint32_t ref, uint32_t mask,
				  uint32_t inv)
{
	amdgpu_ring_write(ring, PACKET3(PACKET3_WAIT_REG_MEM, 5));
	amdgpu_ring_write(ring,
			  /* memory (1) or register (0) */
			  (WAIT_REG_MEM_MEM_SPACE(mem_space) |
			   WAIT_REG_MEM_OPERATION(opt) | /* wait */
			   WAIT_REG_MEM_FUNCTION(3) |  /* equal */
			   WAIT_REG_MEM_ENGINE(eng_sel)));

	if (mem_space)
		BUG_ON(addr0 & 0x3); /* Dword align */
	amdgpu_ring_write(ring, addr0);
	amdgpu_ring_write(ring, addr1);
	amdgpu_ring_write(ring, ref);
	amdgpu_ring_write(ring, mask);
	amdgpu_ring_write(ring, inv); /* poll interval */
}

static bool gfx_v10_0_check_done(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct drm_gpu_scheduler *sched = &ring->sched;
	struct drm_sched_job *s_job;
	struct amdgpu_job *job;
	uint64_t rptr, wptr;
	uint32_t engine;

	spin_lock(&sched->job_list_lock);
	list_for_each_entry(s_job, &sched->pending_list, list) {
		struct dma_fence *fence= s_job->s_fence->parent;

		if (!fence)
			continue;
		job = to_amdgpu_job(s_job);
		DRM_INFO("%s: vmid %u job_id %lld FENCE drm %lld/%lld/%lld sgpu %lld/%lld\n",
			 ring->name, job->vmid, s_job->id,
			 s_job->s_fence->scheduled.context,
			 s_job->s_fence->finished.context,
			 s_job->s_fence->finished.seqno,
			 fence->context, fence->seqno);
	}
	spin_unlock(&sched->job_list_lock);

	DRM_INFO("%s: %s unified_fw_version %u.%u.%u RTL_CL %u\n",
		 __func__, ring->name,
		adev->gfx.sgpu_fw_major_version,
		adev->gfx.sgpu_fw_minor_version,
		adev->gfx.sgpu_fw_option_version,
		adev->gfx.sgpu_rtl_cl_number);

	mutex_lock(&adev->srbm_mutex);
	nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
	rptr = ring->funcs->get_rptr(ring);
	wptr = ring->funcs->get_wptr(ring);
	DRM_INFO("%s: %s rptr %x ib base %llx ptr %x mptr 0x%llx/%llx/%llx_m%x seq %u/%u/%u_m%x\n",
		 __func__,
		 ring->name,
		 ring->me ?
		 RREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR) :
		 RREG32_SOC15(GC, 0, mmCP_RB0_RPTR),
		 ring->me ?
		 ((uint64_t)RREG32_SOC15(GC, 0, mmCP_HQD_IB_BASE_ADDR_HI) << 32)|
		 RREG32_SOC15(GC, 0, mmCP_HQD_IB_BASE_ADDR) :
		 ((uint64_t)RREG32_SOC15(GC, 0, mmCP_IB1_BASE_HI) << 32)|
		 RREG32_SOC15(GC, 0, mmCP_IB1_BASE_LO),
		 ring->me ?
		 RREG32_SOC15(GC, 0, mmCP_HQD_IB_RPTR) :
		 RREG32_SOC15(GC, 0, mmCP_IB1_CMD_BUFSZ) -
		 RREG32_SOC15(GC, 0, mmCP_IB1_BUFSZ),
		 rptr, wptr & ring->buf_mask,
		 ring->wptr,
		 ring->buf_mask,
		 atomic_read(&ring->fence_drv.last_seq),
		 *ring->fence_drv.cpu_addr,
		 ring->fence_drv.sync_seq,
		 ring->fence_drv.num_fences_mask);

	DRM_INFO("%s: %s pfp %08x %08x %08x %08x %08x %08x %08x %08x\n",
		 __func__, ring->name,
		 RREG32_SOC15(GC, 0, mmCP_PFP_HEADER_DUMP),
		 RREG32_SOC15(GC, 0, mmCP_PFP_HEADER_DUMP),
		 RREG32_SOC15(GC, 0, mmCP_PFP_HEADER_DUMP),
		 RREG32_SOC15(GC, 0, mmCP_PFP_HEADER_DUMP),
		 RREG32_SOC15(GC, 0, mmCP_PFP_HEADER_DUMP),
		 RREG32_SOC15(GC, 0, mmCP_PFP_HEADER_DUMP),
		 RREG32_SOC15(GC, 0, mmCP_PFP_HEADER_DUMP),
		 RREG32_SOC15(GC, 0, mmCP_PFP_HEADER_DUMP));
	engine = ring->me ? SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME1_HEADER_DUMP) :
		SOC15_REG_OFFSET(GC, 0, mmCP_ME_HEADER_DUMP);
	DRM_INFO("%s: %s %s %08x %08x %08x %08x %08x %08x %08x %08x\n",
		 __func__, ring->name,
		 ring->me ? "mec" : "me",
		 RREG32(engine),
		 RREG32(engine),
		 RREG32(engine),
		 RREG32(engine),
		 RREG32(engine),
		 RREG32(engine),
		 RREG32(engine),
		 RREG32(engine));
	DRM_INFO("%s: %s %s_cntl %08x grbm_status %08x/%08x/%08x se %08x\n",
		 __func__, ring->name,
		 ring->me ? "mec" : "me",
		 ring->me ?
		 RREG32_SOC15(GC, 0, mmCP_MEC_CNTL) :
		 RREG32_SOC15(GC, 0, mmCP_ME_CNTL),
		 RREG32_SOC15(GC, 0, mmGRBM_STATUS),
		 RREG32_SOC15(GC, 0, mmGRBM_STATUS2),
		 RREG32_SOC15(GC, 0, mmGRBM_STATUS3),
		 RREG32_SOC15(GC, 0, mmGRBM_STATUS_SE0));
	nv_grbm_select(adev, 0, 0, 0, 0);
	mutex_unlock(&adev->srbm_mutex);

	return rptr == wptr;
}

static int gfx_v10_0_ring_test_ring(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t scratch;
	uint32_t tmp = 0;
	unsigned i;
	int r;

	r = amdgpu_gfx_scratch_get(adev, &scratch);
	if (r) {
		DRM_ERROR("amdgpu: cp failed to get scratch reg (%d).\n", r);
		return r;
	}

	WREG32(scratch, 0xCAFEDEAD);

	r = amdgpu_ring_alloc(ring, 5);
	if (r) {
		DRM_ERROR("amdgpu: cp failed to lock ring %d (%d).\n",
			  ring->idx, r);
		amdgpu_gfx_scratch_free(adev, scratch);
		return r;
	}

	amdgpu_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, scratch);
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, 0xDEADBEEF);
	amdgpu_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = RREG32(scratch);
		if (tmp == 0xDEADBEEF)
			break;
		if (amdgpu_emu_mode == 1)
			msleep(1);
		else
			udelay(1);
	}

	if (i >= adev->usec_timeout) {
		gfx_v10_0_check_done(ring);
		r = -ETIMEDOUT;
	}

	amdgpu_gfx_scratch_free(adev, scratch);

	return r;
}

static int gfx_v10_0_ring_test_ib(struct amdgpu_ring *ring, long timeout)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_ib ib;
	struct dma_fence *f = NULL;
	unsigned index;
	uint64_t gpu_addr;
	uint32_t tmp;
	long r;

	r = amdgpu_device_wb_get(adev, &index);
	if (r)
		return r;

	gpu_addr = adev->wb.gpu_addr + (index * 4);
	adev->wb.wb[index] = cpu_to_le32(0xCAFEDEAD);
	memset(&ib, 0, sizeof(ib));
	r = amdgpu_ib_get(adev, NULL, 16,
					AMDGPU_IB_POOL_DIRECT, &ib);
	if (r) {
		DRM_ERROR("amdgpu: failed to get ib (%ld).\n", r);
		goto err1;
	}

	ib.ptr[0] = PACKET3(PACKET3_WRITE_DATA, 3);
	ib.ptr[1] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM;
	ib.ptr[2] = lower_32_bits(gpu_addr);
	ib.ptr[3] = upper_32_bits(gpu_addr);
	ib.ptr[4] = 0xDEADBEEF;
	ib.length_dw = 5;

	r = amdgpu_ib_schedule(ring, 1, &ib, NULL, &f);
	if (r)
		goto err2;

	r = dma_fence_wait_timeout(f, false, timeout);
	if (r == 0) {
		r = -ETIMEDOUT;
		goto err2;
	} else if (r < 0) {
		goto err2;
	}

	tmp = adev->wb.wb[index];
	if (tmp == 0xDEADBEEF)
		r = 0;
	else
		r = -EINVAL;
err2:
	amdgpu_ib_free(adev, &ib, NULL);
	dma_fence_put(f);
err1:
	amdgpu_device_wb_free(adev, index);
	if (r)
		gfx_v10_0_check_done(ring);
	return r;
}

static void gfx_v10_0_free_microcode(struct amdgpu_device *adev)
{
	enum amdgpu_firmware_load_type load_type = adev->firmware.load_type;

	if ((adev->asic_type == CHIP_VANGOGH_LITE) &&
	    (load_type == AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO)) {
		release_firmware(adev->gfx.unified_fw);
		adev->gfx.unified_fw = NULL;
		return;
	}

	release_firmware(adev->gfx.pfp_fw);
	adev->gfx.pfp_fw = NULL;
	release_firmware(adev->gfx.me_fw);
	adev->gfx.me_fw = NULL;
	release_firmware(adev->gfx.ce_fw);
	adev->gfx.ce_fw = NULL;
	release_firmware(adev->gfx.rlc_fw);
	adev->gfx.rlc_fw = NULL;
	release_firmware(adev->gfx.mec_fw);
	adev->gfx.mec_fw = NULL;
	release_firmware(adev->gfx.mec2_fw);
	adev->gfx.mec2_fw = NULL;

	kfree(adev->gfx.rlc.register_list_format);
}

static void gfx_v10_0_check_fw_write_wait(struct amdgpu_device *adev)
{
	adev->gfx.cp_fw_write_wait = false;

	if (!adev->gfx.cp_fw_write_wait)
		DRM_WARN_ONCE("CP firmware version too old, please update!");
}

static void gfx_v10_0_init_rlc_ext_microcode(struct amdgpu_device *adev)
{
	const struct rlc_firmware_header_v2_1 *rlc_hdr;

	rlc_hdr = (const struct rlc_firmware_header_v2_1 *)adev->gfx.rlc_fw->data;
	adev->gfx.rlc_srlc_fw_version = le32_to_cpu(rlc_hdr->save_restore_list_cntl_ucode_ver);
	adev->gfx.rlc_srlc_feature_version = le32_to_cpu(rlc_hdr->save_restore_list_cntl_feature_ver);
	adev->gfx.rlc.save_restore_list_cntl_size_bytes = le32_to_cpu(rlc_hdr->save_restore_list_cntl_size_bytes);
	adev->gfx.rlc.save_restore_list_cntl = (u8 *)rlc_hdr + le32_to_cpu(rlc_hdr->save_restore_list_cntl_offset_bytes);
	adev->gfx.rlc_srlg_fw_version = le32_to_cpu(rlc_hdr->save_restore_list_gpm_ucode_ver);
	adev->gfx.rlc_srlg_feature_version = le32_to_cpu(rlc_hdr->save_restore_list_gpm_feature_ver);
	adev->gfx.rlc.save_restore_list_gpm_size_bytes = le32_to_cpu(rlc_hdr->save_restore_list_gpm_size_bytes);
	adev->gfx.rlc.save_restore_list_gpm = (u8 *)rlc_hdr + le32_to_cpu(rlc_hdr->save_restore_list_gpm_offset_bytes);
	adev->gfx.rlc_srls_fw_version = le32_to_cpu(rlc_hdr->save_restore_list_srm_ucode_ver);
	adev->gfx.rlc_srls_feature_version = le32_to_cpu(rlc_hdr->save_restore_list_srm_feature_ver);
	adev->gfx.rlc.save_restore_list_srm_size_bytes = le32_to_cpu(rlc_hdr->save_restore_list_srm_size_bytes);
	adev->gfx.rlc.save_restore_list_srm = (u8 *)rlc_hdr + le32_to_cpu(rlc_hdr->save_restore_list_srm_offset_bytes);
	adev->gfx.rlc.reg_list_format_direct_reg_list_length =
			le32_to_cpu(rlc_hdr->reg_list_format_direct_reg_list_length);
}

static void gfx_v10_0_init_rlc_iram_dram_microcode(struct amdgpu_device *adev)
{
	const struct rlc_firmware_header_v2_2 *rlc_hdr;

	rlc_hdr = (const struct rlc_firmware_header_v2_2 *)adev->gfx.rlc_fw->data;
	adev->gfx.rlc.rlc_iram_ucode_size_bytes = le32_to_cpu(rlc_hdr->rlc_iram_ucode_size_bytes);
	adev->gfx.rlc.rlc_iram_ucode = (u8 *)rlc_hdr + le32_to_cpu(rlc_hdr->rlc_iram_ucode_offset_bytes);
	adev->gfx.rlc.rlc_dram_ucode_size_bytes = le32_to_cpu(rlc_hdr->rlc_dram_ucode_size_bytes);
	adev->gfx.rlc.rlc_dram_ucode = (u8 *)rlc_hdr + le32_to_cpu(rlc_hdr->rlc_dram_ucode_offset_bytes);
}

static void gfx_v10_0_check_gfxoff_flag(struct amdgpu_device *adev)
{
	/* Nothing to be done for Vangogh */
	return;
}

static int gfx_v10_0_init_microcode(struct amdgpu_device *adev)
{
	const char *chip_name;
	char fw_name[60];
	char wks[10];
	int err;
	const struct gfx_firmware_header_v1_0 *cp_hdr;
	const struct rlc_firmware_header_v2_0 *rlc_hdr;
	unsigned int *tmp = NULL;
	unsigned int i = 0;
	uint16_t version_major;
	uint16_t version_minor;

	if ((adev->asic_type == CHIP_VANGOGH_LITE) &&
	    (adev->firmware.load_type == AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO))
		return 0;

	DRM_DEBUG("\n");

	memset(wks, 0, sizeof(wks));
	switch (adev->asic_type) {
	default:
		BUG();
	}

	snprintf(fw_name, sizeof(fw_name), "amdgpu/%s_pfp%s.bin",
		 chip_name, wks);

	err = request_firmware(&adev->gfx.pfp_fw, fw_name, adev->dev);
	if (err)
		goto out;
	err = amdgpu_ucode_validate(adev->gfx.pfp_fw);
	if (err)
		goto out;
	cp_hdr = (const struct gfx_firmware_header_v1_0 *)adev->gfx.pfp_fw->data;
	adev->gfx.pfp_fw_version = le32_to_cpu(cp_hdr->header.ucode_version);
	adev->gfx.pfp_feature_version = le32_to_cpu(cp_hdr->ucode_feature_version);

	snprintf(fw_name, sizeof(fw_name), "amdgpu/%s_me%s.bin",
		 chip_name, wks);

	err = request_firmware(&adev->gfx.me_fw, fw_name, adev->dev);
	if (err)
		goto out;
	err = amdgpu_ucode_validate(adev->gfx.me_fw);
	if (err)
		goto out;
	cp_hdr = (const struct gfx_firmware_header_v1_0 *)adev->gfx.me_fw->data;
	adev->gfx.me_fw_version = le32_to_cpu(cp_hdr->header.ucode_version);
	adev->gfx.me_feature_version = le32_to_cpu(cp_hdr->ucode_feature_version);

	snprintf(fw_name, sizeof(fw_name), "amdgpu/%s_ce%s.bin",
		 chip_name, wks);
	err = request_firmware(&adev->gfx.ce_fw, fw_name, adev->dev);
	if (err)
		goto out;
	err = amdgpu_ucode_validate(adev->gfx.ce_fw);
	if (err)
		goto out;
	cp_hdr = (const struct gfx_firmware_header_v1_0 *)adev->gfx.ce_fw->data;
	adev->gfx.ce_fw_version = le32_to_cpu(cp_hdr->header.ucode_version);
	adev->gfx.ce_feature_version = le32_to_cpu(cp_hdr->ucode_feature_version);

	if (!amdgpu_sriov_vf(adev)) {
		snprintf(fw_name, sizeof(fw_name), "amdgpu/%s_rlc.bin", chip_name);
		err = request_firmware(&adev->gfx.rlc_fw, fw_name, adev->dev);
		if (err)
			goto out;
		err = amdgpu_ucode_validate(adev->gfx.rlc_fw);
		rlc_hdr = (const struct rlc_firmware_header_v2_0 *)adev->gfx.rlc_fw->data;
		version_major = le16_to_cpu(rlc_hdr->header.header_version_major);
		version_minor = le16_to_cpu(rlc_hdr->header.header_version_minor);

		adev->gfx.rlc_fw_version = le32_to_cpu(rlc_hdr->header.ucode_version);
		adev->gfx.rlc_feature_version = le32_to_cpu(rlc_hdr->ucode_feature_version);
		adev->gfx.rlc.save_and_restore_offset =
			le32_to_cpu(rlc_hdr->save_and_restore_offset);
		adev->gfx.rlc.clear_state_descriptor_offset =
			le32_to_cpu(rlc_hdr->clear_state_descriptor_offset);
		adev->gfx.rlc.avail_scratch_ram_locations =
			le32_to_cpu(rlc_hdr->avail_scratch_ram_locations);
		adev->gfx.rlc.reg_restore_list_size =
			le32_to_cpu(rlc_hdr->reg_restore_list_size);
		adev->gfx.rlc.reg_list_format_start =
			le32_to_cpu(rlc_hdr->reg_list_format_start);
		adev->gfx.rlc.reg_list_format_separate_start =
			le32_to_cpu(rlc_hdr->reg_list_format_separate_start);
		adev->gfx.rlc.starting_offsets_start =
			le32_to_cpu(rlc_hdr->starting_offsets_start);
		adev->gfx.rlc.reg_list_format_size_bytes =
			le32_to_cpu(rlc_hdr->reg_list_format_size_bytes);
		adev->gfx.rlc.reg_list_size_bytes =
			le32_to_cpu(rlc_hdr->reg_list_size_bytes);
		adev->gfx.rlc.register_list_format =
			kmalloc(adev->gfx.rlc.reg_list_format_size_bytes +
					adev->gfx.rlc.reg_list_size_bytes, GFP_KERNEL);
		if (!adev->gfx.rlc.register_list_format) {
			err = -ENOMEM;
			goto out;
		}

		tmp = (unsigned int *)((uintptr_t)rlc_hdr +
							   le32_to_cpu(rlc_hdr->reg_list_format_array_offset_bytes));
		for (i = 0 ; i < (rlc_hdr->reg_list_format_size_bytes >> 2); i++)
			adev->gfx.rlc.register_list_format[i] =	le32_to_cpu(tmp[i]);

		adev->gfx.rlc.register_restore = adev->gfx.rlc.register_list_format + i;

		tmp = (unsigned int *)((uintptr_t)rlc_hdr +
							   le32_to_cpu(rlc_hdr->reg_list_array_offset_bytes));
		for (i = 0 ; i < (rlc_hdr->reg_list_size_bytes >> 2); i++)
			adev->gfx.rlc.register_restore[i] = le32_to_cpu(tmp[i]);

		if (version_major == 2) {
			if (version_minor >= 1)
				gfx_v10_0_init_rlc_ext_microcode(adev);
			if (version_minor == 2)
				gfx_v10_0_init_rlc_iram_dram_microcode(adev);
		}
	}

	snprintf(fw_name, sizeof(fw_name), "amdgpu/%s_mec%s.bin",
		 chip_name, wks);
	err = request_firmware(&adev->gfx.mec_fw, fw_name, adev->dev);
	if (err)
		goto out;
	err = amdgpu_ucode_validate(adev->gfx.mec_fw);
	if (err)
		goto out;
	cp_hdr = (const struct gfx_firmware_header_v1_0 *)adev->gfx.mec_fw->data;
	adev->gfx.mec_fw_version = le32_to_cpu(cp_hdr->header.ucode_version);
	adev->gfx.mec_feature_version = le32_to_cpu(cp_hdr->ucode_feature_version);

	snprintf(fw_name, sizeof(fw_name), "amdgpu/%s_mec2%s.bin",
		 chip_name, wks);
	err = request_firmware(&adev->gfx.mec2_fw, fw_name, adev->dev);
	if (!err) {
		err = amdgpu_ucode_validate(adev->gfx.mec2_fw);
		if (err)
			goto out;
		cp_hdr = (const struct gfx_firmware_header_v1_0 *)
		adev->gfx.mec2_fw->data;
		adev->gfx.mec2_fw_version =
		le32_to_cpu(cp_hdr->header.ucode_version);
		adev->gfx.mec2_feature_version =
		le32_to_cpu(cp_hdr->ucode_feature_version);
	} else {
		err = 0;
		adev->gfx.mec2_fw = NULL;
	}

	gfx_v10_0_check_fw_write_wait(adev);
out:
	if (err) {
		dev_err(adev->dev,
			"gfx10: Failed to load firmware \"%s\"\n",
			fw_name);
		release_firmware(adev->gfx.pfp_fw);
		adev->gfx.pfp_fw = NULL;
		release_firmware(adev->gfx.me_fw);
		adev->gfx.me_fw = NULL;
		release_firmware(adev->gfx.ce_fw);
		adev->gfx.ce_fw = NULL;
		release_firmware(adev->gfx.rlc_fw);
		adev->gfx.rlc_fw = NULL;
		release_firmware(adev->gfx.mec_fw);
		adev->gfx.mec_fw = NULL;
		release_firmware(adev->gfx.mec2_fw);
		adev->gfx.mec2_fw = NULL;
	}

	gfx_v10_0_check_gfxoff_flag(adev);

	return err;
}

static u32 gfx_v10_0_get_csb_size(struct amdgpu_device *adev)
{
	u32 count = 0;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;

	if (adev->family == AMDGPU_FAMILY_MGFX)
		return 0;

	/* begin clear state */
	count += 2;
	/* context control state */
	count += 3;

	for (sect = gfx10_cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT)
				count += 2 + ext->reg_count;
			else
				return 0;
		}
	}

	/* set PA_SC_TILE_STEERING_OVERRIDE */
	count += 3;
	/* end clear state */
	count += 2;
	/* clear state */
	count += 2;

	return count;
}

static void gfx_v10_0_get_csb_buffer(struct amdgpu_device *adev,
				    volatile u32 *buffer)
{
	u32 count = 0, i;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;
	int ctx_reg_offset;

	if (adev->gfx.rlc.cs_data == NULL)
		return;
	if (buffer == NULL)
		return;

	buffer[count++] = cpu_to_le32(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	buffer[count++] = cpu_to_le32(PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);

	buffer[count++] = cpu_to_le32(PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	buffer[count++] = cpu_to_le32(0x80000000);
	buffer[count++] = cpu_to_le32(0x80000000);

	for (sect = adev->gfx.rlc.cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT) {
				buffer[count++] =
					cpu_to_le32(PACKET3(PACKET3_SET_CONTEXT_REG, ext->reg_count));
				buffer[count++] = cpu_to_le32(ext->reg_index -
						PACKET3_SET_CONTEXT_REG_START);
				for (i = 0; i < ext->reg_count; i++)
					buffer[count++] = cpu_to_le32(ext->extent[i]);
			} else {
				return;
			}
		}
	}

	ctx_reg_offset =
		SOC15_REG_OFFSET(GC, 0, mmPA_SC_TILE_STEERING_OVERRIDE) - PACKET3_SET_CONTEXT_REG_START;
	buffer[count++] = cpu_to_le32(PACKET3(PACKET3_SET_CONTEXT_REG, 1));
	buffer[count++] = cpu_to_le32(ctx_reg_offset);
	buffer[count++] = cpu_to_le32(adev->gfx.config.pa_sc_tile_steering_override);

	buffer[count++] = cpu_to_le32(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	buffer[count++] = cpu_to_le32(PACKET3_PREAMBLE_END_CLEAR_STATE);

	buffer[count++] = cpu_to_le32(PACKET3(PACKET3_CLEAR_STATE, 0));
	buffer[count++] = cpu_to_le32(0);
}

static void gfx_v10_0_rlc_fini(struct amdgpu_device *adev)
{
	/* clear state block */
	amdgpu_bo_free_kernel(&adev->gfx.rlc.clear_state_obj,
			&adev->gfx.rlc.clear_state_gpu_addr,
			(void **)&adev->gfx.rlc.cs_ptr);

	/* jump table block */
	amdgpu_bo_free_kernel(&adev->gfx.rlc.cp_table_obj,
			&adev->gfx.rlc.cp_table_gpu_addr,
			(void **)&adev->gfx.rlc.cp_table_ptr);
}

static int gfx_v10_0_rlc_init(struct amdgpu_device *adev)
{
	const struct cs_section_def *cs_data;
	int r;

	if (adev->family == AMDGPU_FAMILY_MGFX)
		return 0;

	adev->gfx.rlc.cs_data = gfx10_cs_data;

	cs_data = adev->gfx.rlc.cs_data;

	if (cs_data) {
		/* init clear state block */
		r = amdgpu_gfx_rlc_init_csb(adev);
		if (r)
			return r;
	}

	/* init spm vmid with 0xf */
	if (adev->gfx.rlc.funcs->update_spm_vmid)
		adev->gfx.rlc.funcs->update_spm_vmid(adev, 0xf);

	return 0;
}

static void gfx_v10_0_mec_fini(struct amdgpu_device *adev)
{
	amdgpu_bo_free_kernel(&adev->gfx.mec.hpd_eop_obj, NULL, NULL);
	amdgpu_bo_free_kernel(&adev->gfx.mec.mec_fw_obj, NULL, NULL);
}

static int gfx_v10_0_me_init(struct amdgpu_device *adev)
{
	int r = 0;

	bitmap_zero(adev->gfx.me.queue_bitmap, AMDGPU_MAX_GFX_QUEUES);

	amdgpu_gfx_graphics_queue_acquire(adev);

	if (adev->asic_type == CHIP_VANGOGH_LITE)
		if (AMDGPU_IS_MGFX0_EVT0(adev->grbm_chip_rev))
			vangogh_lite_gc_clock_gating_workaround(adev);

	return r;
}

/*
 * map the queue by programming registers
 * The caller should guarantee the ring has been initialized
 */
static int gfx_v10_0_pio_map_queue(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_compute_mqd *mqd = ring->mqd_ptr;

	trace_sgpu_pio_map_queue_entry(ring);
	if (amdgpu_in_reset(adev) || adev->in_suspend) {
		ring->wptr = 0;
		amdgpu_ring_clear_ring(ring);
	} else if (!(ring->cwsr || ring->tmz)) {
		amdgpu_ring_clear_ring(ring);
	}

	mutex_lock(&adev->srbm_mutex);
	if (ring->cwsr || ring->tmz)
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue,
			       ring->priv_vmid);
	else
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);

	mqd->cp_hqd_active = 1;
	gfx_v10_0_kiq_init_register(ring);
	nv_grbm_select(adev, 0, 0, 0, 0);
	mutex_unlock(&adev->srbm_mutex);

	trace_sgpu_pio_map_queue_exit(ring, 0);
	DRM_DEBUG_DRIVER("Map ring %s by PIO!\n", ring->name);

	return 0;
}

static int gfx_v10_0_pio_unmap_queue(struct amdgpu_ring *ring,
				     u32 dequeue_type)
{
	u32 tmp, i, timeout;
	struct amdgpu_device *adev;
	struct v10_compute_mqd *mqd;

	if (!ring)
		return 0;

	trace_sgpu_pio_unmap_queue_entry(ring);
	mqd = ring->mqd_ptr;
	adev = ring->adev;

	mutex_lock(&adev->srbm_mutex);

	sgpu_ifpo_lock(adev);

	if (ring->cwsr || ring->tmz) {
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue,
			       ring->priv_vmid);
	} else {
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
	}

	WREG32_SOC15(GC, 0, mmCP_HQD_DEQUEUE_REQUEST,
		     (dequeue_type <<
		      CP_HQD_DEQUEUE_REQUEST__DEQUEUE_REQ__SHIFT) |
		     (1 << CP_HQD_DEQUEUE_REQUEST__DEQUEUE_REQ_EN__SHIFT));

	if (ring->tmz)
		/* tmz workloads might need longer wait time
		 * set it to 2s for now
		 */
		timeout = 2 * 1000 * 1000;
	else
		timeout = adev->usec_timeout;

	for (i = 0; i < timeout; i++) {
		tmp = RREG32(SOC15_REG_OFFSET(GC, 0, mmCP_HQD_ACTIVE));
		tmp &= CP_HQD_ACTIVE__ACTIVE_MASK;
		if (!tmp)
			break;

		udelay(1);
	}

	nv_grbm_select(adev, 0, 0, 0, 0);

	sgpu_ifpo_unlock(adev);

	mutex_unlock(&adev->srbm_mutex);

	if (i == timeout) {
		DRM_WARN("Timeout to unmap queue(%u.%u.%u)\n",
			 ring->me, ring->pipe, ring->queue);
		trace_sgpu_pio_unmap_queue_exit(ring, ETIMEDOUT);
		return -ETIMEDOUT;
	} else if (mqd->cp_hqd_persistent_state &
		   (1 << CP_HQD_PERSISTENT_STATE__RELAUNCH_WAVES__SHIFT))
		DRM_DEBUG_DRIVER("compute waves are saved\n");

	DRM_DEBUG_DRIVER("Unmap ring %s by PIO!\n", ring->name);

	trace_sgpu_pio_unmap_queue_exit(ring, 0);
	return 0;
}

static int gfx_v10_0_mec_init(struct amdgpu_device *adev)
{
	int r;
	u32 *hpd;
	const __le32 *fw_data = NULL;
	unsigned fw_size;
	u32 *fw = NULL;
	size_t mec_hpd_size;

	const struct gfx_firmware_header_v1_0 *mec_hdr = NULL;

	bitmap_zero(adev->gfx.mec.queue_bitmap, AMDGPU_MAX_COMPUTE_QUEUES);

	/* take ownership of the relevant compute queues */
	amdgpu_gfx_compute_queue_acquire(adev);
	mec_hpd_size = adev->gfx.num_compute_rings * GFX10_MEC_HPD_SIZE;

	if (mec_hpd_size) {
		r = amdgpu_bo_create_reserved(adev, mec_hpd_size, PAGE_SIZE,
					      AMDGPU_GEM_DOMAIN_GTT,
					      &adev->gfx.mec.hpd_eop_obj,
					      &adev->gfx.mec.hpd_eop_gpu_addr,
					      (void **)&hpd);
		if (r) {
			dev_warn(adev->dev, "(%d) create HDP EOP bo failed\n", r);
			gfx_v10_0_mec_fini(adev);
			return r;
		}

		memset(hpd, 0, mec_hpd_size);

		amdgpu_bo_kunmap(adev->gfx.mec.hpd_eop_obj);
		amdgpu_bo_unreserve(adev->gfx.mec.hpd_eop_obj);
	}

	/* For VANGOGH_LITE, use PIO to map the KCQ */
	if (adev->asic_type == CHIP_VANGOGH_LITE) {
		adev->gfx.mec.pio_map_queue = gfx_v10_0_pio_map_queue;
		adev->gfx.mec.pio_unmap_queue = gfx_v10_0_pio_unmap_queue;
	}

	if (cwsr_enable || amdgpu_tmz) {
		adev->gfx.mec.map_priv_queue = gfx_v10_0_pio_map_queue;
		adev->gfx.mec.unmap_priv_queue = gfx_v10_0_pio_unmap_queue;
	}

	if ((adev->asic_type == CHIP_VANGOGH_LITE) &&
	    (adev->firmware.load_type == AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO))
		return 0;

	if (adev->firmware.load_type == AMDGPU_FW_LOAD_DIRECT) {
		mec_hdr = (const struct gfx_firmware_header_v1_0 *)adev->gfx.mec_fw->data;

		fw_data = (const __le32 *) (adev->gfx.mec_fw->data +
			 le32_to_cpu(mec_hdr->header.ucode_array_offset_bytes));
		fw_size = le32_to_cpu(mec_hdr->header.ucode_size_bytes);

		r = amdgpu_bo_create_reserved(adev, mec_hdr->header.ucode_size_bytes,
					      PAGE_SIZE, AMDGPU_GEM_DOMAIN_GTT,
					      &adev->gfx.mec.mec_fw_obj,
					      &adev->gfx.mec.mec_fw_gpu_addr,
					      (void **)&fw);
		if (r) {
			dev_err(adev->dev, "(%d) failed to create mec fw bo\n", r);
			gfx_v10_0_mec_fini(adev);
			return r;
		}

		memcpy(fw, fw_data, fw_size);

		amdgpu_bo_kunmap(adev->gfx.mec.mec_fw_obj);
		amdgpu_bo_unreserve(adev->gfx.mec.mec_fw_obj);
	}

	return 0;
}

static uint32_t wave_read_ind(struct amdgpu_device *adev, uint32_t wave, uint32_t address)
{
	WREG32_SOC15(GC, 0, mmSQ_IND_INDEX,
		(wave << SQ_IND_INDEX__WAVE_ID__SHIFT) |
		(address << SQ_IND_INDEX__INDEX__SHIFT));
	return RREG32_SOC15(GC, 0, mmSQ_IND_DATA);
}

static void wave_read_regs(struct amdgpu_device *adev, uint32_t wave,
			   uint32_t thread, uint32_t regno,
			   uint32_t num, uint32_t *out)
{
	WREG32_SOC15(GC, 0, mmSQ_IND_INDEX,
		(wave << SQ_IND_INDEX__WAVE_ID__SHIFT) |
		(regno << SQ_IND_INDEX__INDEX__SHIFT) |
		(thread << SQ_IND_INDEX__WORKITEM_ID__SHIFT) |
		(SQ_IND_INDEX__AUTO_INCR_MASK));
	while (num--)
		*(out++) = RREG32_SOC15(GC, 0, mmSQ_IND_DATA);
}

static void gfx_v10_0_read_wave_data(struct amdgpu_device *adev, uint32_t simd, uint32_t wave, uint32_t *dst, int *no_fields)
{
	/* in gfx10 the SIMD_ID is specified as part of the INSTANCE
	 * field when performing a select_se_sh so it should be
	 * zero here */
	WARN_ON(simd != 0);

	/* type 2 wave data */
	dst[(*no_fields)++] = 2;
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_STATUS);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_PC_LO);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_PC_HI);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_EXEC_LO);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_EXEC_HI);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_HW_ID1);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_HW_ID2);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_INST_DW0);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_GPR_ALLOC);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_LDS_ALLOC);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_TRAPSTS);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_IB_STS);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_IB_STS2);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_IB_DBG1);
	dst[(*no_fields)++] = wave_read_ind(adev, wave, ixSQ_WAVE_M0);
}

static void gfx_v10_0_read_wave_sgprs(struct amdgpu_device *adev, uint32_t simd,
				     uint32_t wave, uint32_t start,
				     uint32_t size, uint32_t *dst)
{
	WARN_ON(simd != 0);

	wave_read_regs(
		adev, wave, 0, start + SQIND_WAVE_SGPRS_OFFSET, size,
		dst);
}

static void gfx_v10_0_read_wave_vgprs(struct amdgpu_device *adev, uint32_t simd,
				      uint32_t wave, uint32_t thread,
				      uint32_t start, uint32_t size,
				      uint32_t *dst)
{
	wave_read_regs(
		adev, wave, thread,
		start + SQIND_WAVE_VGPRS_OFFSET, size, dst);
}

static void gfx_v10_0_select_me_pipe_q(struct amdgpu_device *adev,
									  u32 me, u32 pipe, u32 q, u32 vm)
 {
       nv_grbm_select(adev, me, pipe, q, vm);
 }

static int gfx_v10_0_set_num_clock_on_wgp(struct amdgpu_device *adev,
					u32 num)
{
	u32 data;

	if (!(adev->cg_flags & AMD_CG_SUPPORT_GFX_STATIC_WGP))
		return -EOPNOTSUPP;

	/* num of wgps to be clocked on can not be 0
	 * or greater than available wgps. */
	if (0 == num || adev->gfx.num_wgps < num)
		return -EINVAL;

	/* num of wgps to be clocked on must be equal or
	 * greater than number of wgp always on. */
	if (adev->gfx.num_aon_wgp > num)
		return -ECANCELED;

	if (adev->gfx.num_clock_on_wgp == num)
		return 0;

	adev->gfx.num_clock_on_wgp = num;
	data = RREG32_SOC15(GC, 0, mmRLC_MAX_PG_WGP);
	data = REG_SET_FIELD(data, RLC_MAX_PG_WGP,
			MAX_POWERED_UP_WGP, num);
	WREG32(SOC15_REG_OFFSET(GC, 0, mmRLC_MAX_PG_WGP), data);

	return 0;
}

static void gfx_v10_0_assign_aon_wgp_bitmap(struct amdgpu_device *adev,
					u32 num_aon_wgp)
{
	u32 aon_wgp = num_aon_wgp;
	u32 se_index = 0;
	u32 sh_index = 0;
	u32 bit_shift = 0;

	memset(adev->gfx.wgp_aon_bitmap, 0, sizeof(adev->gfx.wgp_aon_bitmap));
	/* WGP are assigned in the order moving through all sh and
	 * moving from least significant bit to high */
	while (aon_wgp) {
		if (adev->gfx.wgp_active_bitmap[se_index][sh_index] & (1 << bit_shift)) {
			adev->gfx.wgp_aon_bitmap[se_index][sh_index] |= (1 << bit_shift);
			aon_wgp--;
		}
		sh_index++;
		if (sh_index == adev->gfx.config.max_sh_per_se) {
			sh_index = 0;
			se_index++;
		}
		if (se_index == adev->gfx.config.max_shader_engines) {
			se_index = 0;
			bit_shift++;
		}
	}
}

static int gfx_v10_0_set_num_aon_wgp(struct amdgpu_device *adev,
					u32 num)
{
	u32 i, j;
	uint32_t data;
	u32 max_se = adev->gfx.config.max_shader_engines;
	u32 max_sh_per_se = adev->gfx.config.max_sh_per_se;

	if (!(adev->cg_flags & AMD_CG_SUPPORT_GFX_STATIC_WGP))
		return -EOPNOTSUPP;

	if (adev->gfx.num_wgps < num)
		return -EINVAL;

	/* number of always on wgps must be equal or less
	 * than number of wgps to be clock on */
	if (adev->gfx.num_clock_on_wgp < num)
		return -ECANCELED;

	if (adev->gfx.num_aon_wgp == num)
		return 0;

	adev->gfx.num_aon_wgp = num;

	gfx_v10_0_assign_aon_wgp_bitmap(adev, num);

	mutex_lock(&adev->grbm_idx_mutex);
	for (i = 0; i < max_se; i++) {
		for (j = 0; j < max_sh_per_se; j++) {
			amdgpu_gfx_select_se_sh(adev, i, j, 0xffffffff);

			data = RREG32_SOC15(GC, 0, mmRLC_PG_ALWAYS_ON_WGP_MASK);
			if (data != adev->gfx.wgp_aon_bitmap[i][j])
				WREG32_SOC15(GC, 0, mmRLC_PG_ALWAYS_ON_WGP_MASK,
					adev->gfx.wgp_aon_bitmap[i][j]);
		}
	}
	amdgpu_gfx_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);
	mutex_unlock(&adev->grbm_idx_mutex);

	return 0;
}

static void gfx_v10_0_read_status_static_wgp(struct amdgpu_device *adev)
{
	u32 i, j;

	mutex_lock(&adev->grbm_idx_mutex);
	for (i = 0; i < adev->gfx.config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx.config.max_sh_per_se; j++) {
			amdgpu_gfx_select_se_sh(adev, i, j, 0xffffffff);
			adev->gfx.wgp_status_bitmap[i][j] =
				RREG32_SOC15(GC, 0, mmRLC_STATIC_PG_STATUS) &
				adev->gfx.wgp_bitmask;
		}
	}
	amdgpu_gfx_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);
	mutex_unlock(&adev->grbm_idx_mutex);
}

static int gfx_v10_0_sw_workaround(struct amdgpu_device *adev,
				    enum amdgpu_gfx_workaround wa,
				    uint32_t data)
{
	switch (wa) {
	case WA_CG_PERFCOUNTER:
		if (adev->asic_type == CHIP_VANGOGH_LITE) {
			vangogh_lite_gc_perfcounter_cg_workaround(adev, data != 0);
			DRM_INFO("Perfcounter workaround %s!",(data ? "Enabled" : "Disabled"));
		}
		break;
	case WA_CG_SQ_THREAD_TRACE:
		/* Synopsys: Thread Trace is in process of tracing and
		 * sending token to memory and RCL is in the process of
		 * clock gating which cause Thread Trace misses the credit
		 * return from CH.  Without full credits restored, SQG hangs.
		 *
		 * Symptoms: SQG hangs while RLC is changing clocks.
		 *
		 * Workaround: Disable CGCG clock gating when Thread Trace
		 * is active.
		 */
		if (adev->asic_type == CHIP_VANGOGH_LITE) {
			gfx_v10_0_update_gfx_CGCG(adev, !data);
			DRM_INFO("SQTT workaround %s!",(data ? "Enabled" : "Disabled"));
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct amdgpu_gfx_funcs gfx_v10_0_gfx_funcs = {
	.get_gpu_clock_counter = &gfx_v10_0_get_gpu_clock_counter,
	.select_se_sh = &gfx_v10_0_select_se_sh,
	.read_wave_data = &gfx_v10_0_read_wave_data,
	.read_wave_sgprs = &gfx_v10_0_read_wave_sgprs,
	.read_wave_vgprs = &gfx_v10_0_read_wave_vgprs,
	.select_me_pipe_q = &gfx_v10_0_select_me_pipe_q,
	.init_spm_golden = &gfx_v10_0_init_spm_golden_registers,
	.set_num_clock_on_wgp = &gfx_v10_0_set_num_clock_on_wgp,
	.set_num_aon_wgp = &gfx_v10_0_set_num_aon_wgp,
	.read_status_static_wgp = &gfx_v10_0_read_status_static_wgp,
	.sw_workaround = &gfx_v10_0_sw_workaround,
};

static void gfx_v10_0_gpu_early_init(struct amdgpu_device *adev)
{
	u32 gb_addr_config;

	adev->gfx.funcs = &gfx_v10_0_gfx_funcs;

	if (0 < MGFX_GEN(adev->grbm_chip_rev) && MGFX_GEN(adev->grbm_chip_rev) <= 2) {
		/* For M1 & M2, the WGP num should be read from register
		 * mmCC_GC_SHADER_ARRAY_CONFIG while not the gpu_info FW.
		 */
		uint32_t wgp = RREG32_SOC15(GC, 0,
				mmCC_GC_SHADER_ARRAY_CONFIG);
		wgp &= GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK;
		wgp = hweight32(wgp);
		/* See reg spec for mmCC_GC_SHADER_ARRAY_CONFIG
		 * [31:16]: Bit-mask of which Work Group Processors are inactive
		 * 	0 = active, 1 = inactive
		 * So the total active WGP number is equal to (16 - wgp)
		 */
		adev->gfx.config.max_cu_per_sh = 2 * ( 16 - wgp);
	}

	switch (adev->asic_type) {
	case CHIP_VANGOGH_LITE:
		adev->gfx.config.max_hw_contexts = 8;
		adev->gfx.config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx.config.sc_prim_fifo_size_backend = 0x100;
		adev->gfx.config.sc_hiz_tile_fifo_size = 0;
		adev->gfx.config.sc_earlyz_tile_fifo_size = 0x4C0;
		if (sgpu_no_hw_access != 0)
			gb_addr_config = GB_ADDR_CONFIG_VAL;
		else
			gb_addr_config = RREG32_SOC15(GC, 0, mmGB_ADDR_CONFIG);
		break;
	default:
		BUG();
		break;
	}

	adev->gfx.config.gb_addr_config = gb_addr_config;

	adev->gfx.config.gb_addr_config_fields.num_pipes = 1 <<
			REG_GET_FIELD(adev->gfx.config.gb_addr_config,
				      GB_ADDR_CONFIG, NUM_PIPES);

	adev->gfx.config.max_tile_pipes =
		adev->gfx.config.gb_addr_config_fields.num_pipes;

	adev->gfx.config.gb_addr_config_fields.max_compress_frags = 1 <<
			REG_GET_FIELD(adev->gfx.config.gb_addr_config,
				      GB_ADDR_CONFIG, MAX_COMPRESSED_FRAGS);
	adev->gfx.config.gb_addr_config_fields.pipe_interleave_size = 1 << (8 +
			REG_GET_FIELD(adev->gfx.config.gb_addr_config,
				      GB_ADDR_CONFIG, PIPE_INTERLEAVE_SIZE));

	/* The below fields are deprecated for SGPU, and this information is
	 * instead queried from FW. Set these to invalid values.
	 */
	adev->gfx.config.gb_addr_config_fields.num_rb_per_se = 0xff;
	adev->gfx.config.gb_addr_config_fields.num_se = 0xff;

	DRM_INFO("GB_ADDR_CONFIG info: num_pipes %u, max_compress_frags %u, "
		 "num_rb_per_se %u, num_se %u, pipe_interleave_size %u\n",
		 adev->gfx.config.gb_addr_config_fields.num_pipes,
		 adev->gfx.config.gb_addr_config_fields.max_compress_frags,
		 adev->gfx.config.gb_addr_config_fields.num_rb_per_se,
		 adev->gfx.config.gb_addr_config_fields.num_se,
		 adev->gfx.config.gb_addr_config_fields.pipe_interleave_size);
}

static int gfx_v10_0_gfx_ring_prio(int me, int pipe, int queue)
{
	unsigned int hw_prio = AMDGPU_GFX_RING_PRIO_LOW;

	switch (me) {
	case 0:
		switch (pipe) {
		case 0:
			switch (queue) {
			case 0:
				hw_prio = AMDGPU_GFX_RING_PRIO_HIGH;
				break;
			}
		}
	}

	return hw_prio;
}

static int gfx_v10_0_gfx_ring_init(struct amdgpu_device *adev, int ring_id,
				   int me, int pipe, int queue)
{
	int r;
	struct amdgpu_ring *ring;
	unsigned int irq_type;
	unsigned int hw_prio = AMDGPU_RING_PRIO_DEFAULT;

	ring = &adev->gfx.gfx_ring[ring_id];

	ring->me = me;
	ring->pipe = pipe;
	ring->queue = queue;

	ring->ring_obj = NULL;
	ring->use_doorbell = true;
	ring->use_pollfence = amdgpu_poll_eop;

	ring->doorbell_index = (adev->doorbell_index.gfx_ring0 + ring_id) << 1;

	sprintf(ring->name, "gfx_%d.%d.%d", ring->me, ring->pipe, ring->queue);

	irq_type = AMDGPU_CP_IRQ_GFX_ME0_PIPE0_EOP + ring->pipe;

	hw_prio = gfx_v10_0_gfx_ring_prio(me, pipe, queue);

	r = amdgpu_ring_init(adev, ring, 1024,
			     &adev->gfx.eop_irq, irq_type,
			     hw_prio);

	if (hw_prio == AMDGPU_GFX_RING_PRIO_HIGH) {
		mutex_lock(&ring->priority_mutex);
		ring->priority = DRM_SCHED_PRIORITY_HIGH;
		mutex_unlock(&ring->priority_mutex);
	}

	if (r)
		return r;
	return 0;
}

static int gfx_v10_0_compute_ring_init(struct amdgpu_device *adev, int ring_id,
				       int mec, int pipe, int queue)
{
	int r;
	unsigned irq_type;
	struct amdgpu_ring *ring;
	unsigned int hw_prio;

	ring = &adev->gfx.compute_ring[ring_id];

	/* mec0 is me1 */
	ring->me = mec + 1;
	ring->pipe = pipe;
	ring->queue = queue;

	ring->ring_obj = NULL;
	ring->use_doorbell = true;
	ring->use_pollfence = amdgpu_poll_eop;
	ring->doorbell_index = (adev->doorbell_index.mec_ring0 + ring_id) << 1;
	ring->eop_gpu_addr = adev->gfx.mec.hpd_eop_gpu_addr
				+ (ring_id * GFX10_MEC_HPD_SIZE);
	sprintf(ring->name, "comp_%d.%d.%d", ring->me, ring->pipe, ring->queue);

	irq_type = AMDGPU_CP_IRQ_COMPUTE_MEC1_PIPE0_EOP
		+ ((ring->me - 1) * adev->gfx.mec.num_pipe_per_mec)
		+ ring->pipe;
	hw_prio = amdgpu_gfx_is_high_priority_compute_queue(adev, ring->pipe,
							    ring->queue) ?
			AMDGPU_GFX_PIPE_PRIO_HIGH : AMDGPU_GFX_PIPE_PRIO_NORMAL;
	/* type-2 packets are deprecated on MEC, use type-3 instead */
	r = amdgpu_ring_init(adev, ring, 1024,
			     &adev->gfx.eop_irq, irq_type, hw_prio);
	if (r)
		return r;

	if (ring->queue == 3) {
		adev->secure_ring = ring;
	}
	return 0;
}

static int gfx_v10_0_sw_init(void *handle)
{
	int i, j, k, r, ring_id = 0;
	struct amdgpu_kiq *kiq;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	switch (adev->asic_type) {
	case CHIP_VANGOGH_LITE:
		/* 4 queues on pipe0 of me */
		adev->gfx.me.num_me = 1;
		adev->gfx.me.num_pipe_per_me = 1;
		adev->gfx.me.num_queue_per_pipe = 4;
		/* 4 queues on pipe0 of mec0 */
		adev->gfx.mec.num_mec = 1;
		adev->gfx.mec.num_pipe_per_mec = 1;
		adev->gfx.mec.num_queue_per_pipe = 4;
		break;
	default:
		adev->gfx.me.num_me = 1;
		adev->gfx.me.num_pipe_per_me = 1;
		adev->gfx.me.num_queue_per_pipe = 1;
		adev->gfx.mec.num_mec = 1;
		adev->gfx.mec.num_pipe_per_mec = 4;
		adev->gfx.mec.num_queue_per_pipe = 8;
		break;
	}

	/* KIQ event not supported by Vangogh */
	DRM_INFO("Skip KIQ event!\n");

	/* EOP Event */
	if (!amdgpu_poll_eop) {
		r = amdgpu_irq_add_id(adev, SOC15_IH_CLIENTID_GRBM_CP,
				GFX_10_1__SRCID__CP_EOP_INTERRUPT,
				&adev->gfx.eop_irq);
		if (r)
				return r;
	}

	/* Privileged reg */
	r = amdgpu_irq_add_id(adev, SOC15_IH_CLIENTID_GRBM_CP, GFX_10_1__SRCID__CP_PRIV_REG_FAULT,
			      &adev->gfx.priv_reg_irq);
	if (r)
		return r;

	/* Privileged inst */
	r = amdgpu_irq_add_id(adev, SOC15_IH_CLIENTID_GRBM_CP, GFX_10_1__SRCID__CP_PRIV_INSTR_FAULT,
			      &adev->gfx.priv_inst_irq);
	if (r)
		return r;

	adev->gfx.gfx_current_status = AMDGPU_GFX_NORMAL_MODE;

	gfx_v10_0_scratch_init(adev);

	r = gfx_v10_0_me_init(adev);
	if (r)
		return r;

	r = gfx_v10_0_rlc_init(adev);
	if (r) {
		DRM_ERROR("Failed to init rlc BOs!\n");
		return r;
	}

	r = gfx_v10_0_mec_init(adev);
	if (r) {
		DRM_ERROR("Failed to init MEC BOs!\n");
		return r;
	}

	/* set up the gfx ring */
	for (i = 0; i < adev->gfx.me.num_me; i++) {
		for (j = 0; j < adev->gfx.me.num_queue_per_pipe; j++) {
			for (k = 0; k < adev->gfx.me.num_pipe_per_me; k++) {
				if (!amdgpu_gfx_is_me_queue_enabled(adev, i, k, j))
					continue;

				r = gfx_v10_0_gfx_ring_init(adev, ring_id,
							    i, k, j);
				if (r)
					return r;
				ring_id++;
			}
		}
	}

	if (adev->gfx.num_compute_rings > 0) {
		ring_id = 0;
		/* set up the compute queues - allocate horizontally across pipes */
		for (i = 0; i < adev->gfx.mec.num_mec; ++i) {
			for (j = 0; j < adev->gfx.mec.num_queue_per_pipe; j++) {
				for (k = 0; k < adev->gfx.mec.num_pipe_per_mec; k++) {
					if (!amdgpu_gfx_is_mec_queue_enabled(adev, i, k,
									     j))
						continue;

					r = gfx_v10_0_compute_ring_init(adev, ring_id,
									i, k, j);
					if (r)
						return r;

					ring_id++;
				}
			}
		}
	}

	if (adev->asic_type != CHIP_VANGOGH_LITE) {
		r = amdgpu_gfx_kiq_init(adev, GFX10_MEC_HPD_SIZE);
		if (r) {
			DRM_ERROR("Failed to init KIQ BOs!\n");
			return r;
		}

		kiq = &adev->gfx.kiq;
		r = amdgpu_gfx_kiq_init_ring(adev, &kiq->ring, &kiq->irq);
		if (r)
			return r;
	} else
		DRM_INFO("Skip KIQ and KIQ ring init!\n");

	r = amdgpu_gfx_mqd_sw_init(adev, sizeof(struct v10_compute_mqd));
	if (r)
		return r;

	if (adev->asic_type == CHIP_VANGOGH_LITE ||
	    amdgpu_eval_mode != 0)
		adev->gfx.ce_ram_size = 0;
	else
		adev->gfx.ce_ram_size = F32_CE_PROGRAM_RAM_SIZE;

	gfx_v10_0_gpu_early_init(adev);

	gfx_v10_0_set_bpmd_funcs(adev);
	sgpu_bpmd_log_init(adev);

	amdgpu_sws_init(adev);

	return 0;
}

static void gfx_v10_0_pfp_fini(struct amdgpu_device *adev)
{
	amdgpu_bo_free_kernel(&adev->gfx.pfp.pfp_fw_obj,
			      &adev->gfx.pfp.pfp_fw_gpu_addr,
			      (void **)&adev->gfx.pfp.pfp_fw_ptr);
}

static void gfx_v10_0_ce_fini(struct amdgpu_device *adev)
{
	amdgpu_bo_free_kernel(&adev->gfx.ce.ce_fw_obj,
			      &adev->gfx.ce.ce_fw_gpu_addr,
			      (void **)&adev->gfx.ce.ce_fw_ptr);
}

static void gfx_v10_0_me_fini(struct amdgpu_device *adev)
{
	amdgpu_bo_free_kernel(&adev->gfx.me.me_fw_obj,
			      &adev->gfx.me.me_fw_gpu_addr,
			      (void **)&adev->gfx.me.me_fw_ptr);
}

static int gfx_v10_0_sw_fini(void *handle)
{
	int i;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	amdgpu_sws_deinit(adev);

	for (i = 0; i < adev->gfx.num_gfx_rings; i++)
		amdgpu_ring_fini(&adev->gfx.gfx_ring[i]);
	for (i = 0; i < adev->gfx.num_compute_rings; i++)
		amdgpu_ring_fini(&adev->gfx.compute_ring[i]);

	amdgpu_gfx_mqd_sw_fini(adev);

	if (adev->asic_type != CHIP_VANGOGH_LITE) {
		amdgpu_gfx_kiq_free_ring(&adev->gfx.kiq.ring);
		amdgpu_gfx_kiq_fini(adev);
	} else
		DRM_INFO("Skip KIQ and KIQ ring fini!\n");

	gfx_v10_0_pfp_fini(adev);
	if (adev->asic_type != CHIP_VANGOGH_LITE)
		gfx_v10_0_ce_fini(adev);
	gfx_v10_0_me_fini(adev);
	gfx_v10_0_rlc_fini(adev);
	gfx_v10_0_mec_fini(adev);

	if (adev->firmware.load_type == AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO)
		gfx_v10_0_rlc_backdoor_autoload_buffer_fini(adev);

	gfx_v10_0_free_microcode(adev);

	return 0;
}

static u32 gfx_v10_0_select_se_sh(struct amdgpu_device *adev, u32 se_num,
				   u32 sh_num, u32 instance)
{
	u32 data;

	if (instance == 0xffffffff)
		data = REG_SET_FIELD(0, GRBM_GFX_INDEX,
				     INSTANCE_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_INDEX,
				     instance);

	if (se_num == 0xffffffff)
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_BROADCAST_WRITES,
				     1);
	else
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_INDEX, se_num);

	if (sh_num == 0xffffffff)
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_BROADCAST_WRITES,
				     1);
	else
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_INDEX, sh_num);

	WREG32_SOC15(GC, 0, mmGRBM_GFX_INDEX, data);

	return data;
}

static u32 gfx_v10_0_get_rb_active_bitmap(struct amdgpu_device *adev)
{
	u32 data, mask;

	data = RREG32_SOC15(GC, 0, mmCC_RB_BACKEND_DISABLE);
	data |= RREG32_SOC15(GC, 0, mmGC_USER_RB_BACKEND_DISABLE);

	data &= CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK;
	data >>= GC_USER_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT;

	mask = amdgpu_gfx_create_bitmask(adev->gfx.config.max_backends_per_se /
					 adev->gfx.config.max_sh_per_se);

	return (~data) & mask;
}

static void gfx_v10_0_setup_rb(struct amdgpu_device *adev)
{
	int i, j;
	u32 data;
	u32 active_rbs = 0;
	u32 bitmap;
	u32 rb_bitmap_width_per_sh = adev->gfx.config.max_backends_per_se /
					adev->gfx.config.max_sh_per_se;

	mutex_lock(&adev->grbm_idx_mutex);
	for (i = 0; i < adev->gfx.config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx.config.max_sh_per_se; j++) {
			bitmap = i * adev->gfx.config.max_sh_per_se + j;
			gfx_v10_0_select_se_sh(adev, i, j, 0xffffffff);
			data = gfx_v10_0_get_rb_active_bitmap(adev);
			active_rbs |= data << ((i * adev->gfx.config.max_sh_per_se + j) *
					       rb_bitmap_width_per_sh);
		}
	}
	gfx_v10_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);
	mutex_unlock(&adev->grbm_idx_mutex);

	adev->gfx.config.backend_enable_mask = active_rbs;
	adev->gfx.config.num_rbs = hweight32(active_rbs);
}

static u32 gfx_v10_0_init_pa_sc_tile_steering_override(struct amdgpu_device *adev)
{
	uint32_t num_sc;
	uint32_t enabled_rb_per_sh;
	uint32_t active_rb_bitmap;
	uint32_t num_rb_per_sc;
	uint32_t num_packer_per_sc;
	uint32_t pa_sc_tile_steering_override;

	/* init num_sc */
	num_sc = adev->gfx.config.max_shader_engines
		* adev->gfx.config.max_sh_per_se
		* adev->gfx.config.num_sc_per_sh;
	/* init num_rb_per_sc */
	active_rb_bitmap = gfx_v10_0_get_rb_active_bitmap(adev);
	enabled_rb_per_sh = hweight32(active_rb_bitmap);
	num_rb_per_sc = enabled_rb_per_sh / adev->gfx.config.num_sc_per_sh;
	/* init num_packer_per_sc */
	num_packer_per_sc = adev->gfx.config.num_packer_per_sc;

	pa_sc_tile_steering_override = 0;
	pa_sc_tile_steering_override |=
		(order_base_2(num_sc) << PA_SC_TILE_STEERING_OVERRIDE__NUM_SC__SHIFT) &
		PA_SC_TILE_STEERING_OVERRIDE__NUM_SC_MASK;
	pa_sc_tile_steering_override |=
		(order_base_2(num_rb_per_sc) << PA_SC_TILE_STEERING_OVERRIDE__NUM_RB_PER_SC__SHIFT) &
		PA_SC_TILE_STEERING_OVERRIDE__NUM_RB_PER_SC_MASK;
	pa_sc_tile_steering_override |=
		(order_base_2(num_packer_per_sc) << PA_SC_TILE_STEERING_OVERRIDE__NUM_PACKER_PER_SC__SHIFT) &
		PA_SC_TILE_STEERING_OVERRIDE__NUM_PACKER_PER_SC_MASK;

	return pa_sc_tile_steering_override;
}

#define DEFAULT_SH_MEM_BASES	(0x6000)

static void gfx_v10_0_init_gds_vmid(struct amdgpu_device *adev)
{
	int vmid;

	/*
	 * Initialize all compute and user-gfx VMIDs to have no GDS, GWS, or OA
	 * access. Compute VMIDs should be enabled by FW for target VMIDs,
	 * the driver can enable them for graphics. VMID0 should maintain
	 * access so that HWS firmware can save/restore entries.
	 */
	for (vmid = 1; vmid < 16; vmid++) {
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_VMID0_BASE, 2 * vmid, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_VMID0_SIZE, 2 * vmid, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_GWS_VMID0, vmid, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_OA_VMID0, vmid, 0);
	}
}


static void gfx_v10_0_tcp_harvest(struct amdgpu_device *adev)
{
	/* This was needed only for legacy NAVIx chips, return for Vangogh */
	return;
}

static void gfx_v10_0_get_tcc_info(struct amdgpu_device *adev)
{
	/* TCCs are global (not instanced). */
	uint32_t tcc_disable = RREG32_SOC15(GC, 0, mmCGTS_TCC_DISABLE) |
			       RREG32_SOC15(GC, 0, mmCGTS_USER_TCC_DISABLE);

	adev->gfx.config.tcc_disabled_mask =
		REG_GET_FIELD(tcc_disable, CGTS_TCC_DISABLE, TCC_DISABLE) |
		(REG_GET_FIELD(tcc_disable, CGTS_TCC_DISABLE, HI_TCC_DISABLE) << 16);
}

static int gfx_v10_0_reconf_se(struct amdgpu_device *adev)
{
	struct amdgpu_gfx_config *config;

	if (amdgpu_eval_mode >= eval_mode_config_end
	    || amdgpu_eval_mode <= eval_mode_config_disabled)
		return -1;

	config = &adev->gfx.config;
	printk("original setting: rbs_per_sa(%u), se_count(%u) sas_per_se(%u)\n"
	       "\tpackers_per_sc(%u) max_cu_per_sh(%u)\n",
	       config->max_backends_per_se / config->max_sh_per_se,
	       config->max_shader_engines,
	       config->max_sh_per_se,
	       config->num_packer_per_sc,
	       config->max_cu_per_sh);

	config->num_packer_per_sc = gc_cfg_tb[amdgpu_eval_mode].packers_per_sc;
	config->max_sh_per_se = gc_cfg_tb[amdgpu_eval_mode].sas_per_se;
	config->max_shader_engines = gc_cfg_tb[amdgpu_eval_mode].se_count;
	config->max_backends_per_se = gc_cfg_tb[amdgpu_eval_mode].rbs_per_sa *
		adev->gfx.config.max_sh_per_se;
	config->max_cu_per_sh = gc_cfg_tb[amdgpu_eval_mode].wgps_per_sa << 1;

	printk("override setting: rbs_per_sa(%u), se_count(%u) sas_per_se(%u)\n"
	       "\tpackers_per_sc(%u) max_cu_per_sh(%u)\n",
	       config->max_backends_per_se / config->max_sh_per_se,
	       config->max_shader_engines,
	       config->max_sh_per_se,
	       config->num_packer_per_sc,
	       config->max_cu_per_sh);

	return 0;
}

static void gfx_v10_0_constants_init(struct amdgpu_device *adev)
{
	u32 tmp;
	int i;

	WREG32_FIELD15(GC, 0, GRBM_CNTL, READ_TIMEOUT, 0xff);

	gfx_v10_0_setup_rb(adev);
	gfx_v10_0_get_cu_info(adev, &adev->gfx.cu_info);

	/* set static wgp clock gating variables to default */
	gfx_v10_0_static_wgp_clockgating_init(adev);

	gfx_v10_0_get_tcc_info(adev);
	adev->gfx.config.pa_sc_tile_steering_override =
		gfx_v10_0_init_pa_sc_tile_steering_override(adev);

	/* XXX SH_MEM regs */
	/* where to put LDS, scratch, GPUVM in FSA64 space */
	mutex_lock(&adev->srbm_mutex);
	for (i = 0; i < adev->vm_manager.id_mgr[AMDGPU_GFXHUB_0].num_ids; i++) {
		nv_grbm_select(adev, 0, 0, 0, i);
		/* CP and shaders */
		if (adev->asic_type == CHIP_VANGOGH_LITE)
			WREG32_SOC15(GC, 0, mmSH_MEM_CONFIG,
				     mmSH_MEM_CONFIG_DEFAULT);
		else
			WREG32_SOC15(GC, 0, mmSH_MEM_CONFIG,
				     DEFAULT_SH_MEM_CONFIG);
		if (i != 0) {
			tmp = REG_SET_FIELD(0, SH_MEM_BASES, PRIVATE_BASE,
				(adev->gmc.private_aperture_start >> 48));
			tmp = REG_SET_FIELD(tmp, SH_MEM_BASES, SHARED_BASE,
				(adev->gmc.shared_aperture_start >> 48));
			WREG32_SOC15(GC, 0, mmSH_MEM_BASES, tmp);
		}
	}
	nv_grbm_select(adev, 0, 0, 0, 0);

	mutex_unlock(&adev->srbm_mutex);

	if (adev->asic_type != CHIP_VANGOGH_LITE)
		gfx_v10_0_init_gds_vmid(adev);
}

static void gfx_v10_0_enable_gui_idle_interrupt(struct amdgpu_device *adev,
					       bool enable)
{
	u32 tmp;

	if (amdgpu_sriov_vf(adev))
		return;

	tmp = RREG32_SOC15(GC, 0, mmCP_INT_CNTL_RING0);

	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CNTX_BUSY_INT_ENABLE,
			    enable ? 1 : 0);
	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CNTX_EMPTY_INT_ENABLE,
			    enable ? 1 : 0);
	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CMP_BUSY_INT_ENABLE,
			    enable ? 1 : 0);
	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, GFX_IDLE_INT_ENABLE,
			    enable ? 1 : 0);

	WREG32_SOC15(GC, 0, mmCP_INT_CNTL_RING0, tmp);
}

static int gfx_v10_0_init_csb(struct amdgpu_device *adev)
{
	if (adev->family == AMDGPU_FAMILY_MGFX)
		return 0;

	adev->gfx.rlc.funcs->get_csb_buffer(adev, adev->gfx.rlc.cs_ptr);

	/* csib */
	WREG32_SOC15(GC, 0, mmRLC_CSIB_ADDR_HI,
			adev->gfx.rlc.clear_state_gpu_addr >> 32);
	WREG32_SOC15(GC, 0, mmRLC_CSIB_ADDR_LO,
			adev->gfx.rlc.clear_state_gpu_addr & 0xfffffffc);
	WREG32_SOC15(GC, 0, mmRLC_CSIB_LENGTH, adev->gfx.rlc.clear_state_size);

	return 0;
}

void gfx_v10_0_rlc_stop(struct amdgpu_device *adev)
{
	u32 tmp = RREG32_SOC15(GC, 0, mmRLC_CNTL);

	tmp = REG_SET_FIELD(tmp, RLC_CNTL, RLC_ENABLE_F32, 0);
	WREG32_SOC15(GC, 0, mmRLC_CNTL, tmp);
}

static void gfx_v10_0_rlc_reset(struct amdgpu_device *adev)
{
	WREG32_FIELD15(GC, 0, GRBM_SOFT_RESET, SOFT_RESET_RLC, 1);
	udelay(50);
	WREG32_FIELD15(GC, 0, GRBM_SOFT_RESET, SOFT_RESET_RLC, 0);
	udelay(50);
}

static void gfx_v10_0_rlc_smu_handshake_cntl(struct amdgpu_device *adev,
					     bool enable)
{
	uint32_t rlc_pg_cntl;

	rlc_pg_cntl = RREG32_SOC15(GC, 0, mmRLC_PG_CNTL);

	if (!enable) {
		/* RLC_PG_CNTL[23] = 0 (default)
		 * RLC will wait for handshake acks with SMU
		 * GFXOFF will be enabled
		 * RLC_PG_CNTL[23] = 1
		 * RLC will not issue any message to SMU
		 * hence no handshake between SMU & RLC
		 * GFXOFF will be disabled
		 */
		rlc_pg_cntl |= 0x800000;
	} else
		rlc_pg_cntl &= ~0x800000;
	WREG32_SOC15(GC, 0, mmRLC_PG_CNTL, rlc_pg_cntl);
}

static void gfx_v10_0_rlc_start(struct amdgpu_device *adev)
{
	/* TODO: enable rlc & smu handshake until smu
	 * and gfxoff feature works as expected */
	if (!(amdgpu_pp_feature_mask & PP_GFXOFF_MASK))
		gfx_v10_0_rlc_smu_handshake_cntl(adev, false);

	WREG32_FIELD15(GC, 0, RLC_CNTL, RLC_ENABLE_F32, 1);
	udelay(50);
}

static void gfx_v10_0_rlc_enable_srm(struct amdgpu_device *adev)
{
	uint32_t tmp;

	/* enable Save Restore Machine */
	tmp = RREG32(SOC15_REG_OFFSET(GC, 0, mmRLC_SRM_CNTL));
	tmp |= RLC_SRM_CNTL__AUTO_INCR_ADDR_MASK;
	tmp |= RLC_SRM_CNTL__SRM_ENABLE_MASK;
	WREG32(SOC15_REG_OFFSET(GC, 0, mmRLC_SRM_CNTL), tmp);
}

static int gfx_v10_0_rlc_load_microcode(struct amdgpu_device *adev)
{
	const struct rlc_firmware_header_v2_0 *hdr;
	const __le32 *fw_data;
	unsigned i, fw_size;

	if (!adev->gfx.rlc_fw)
		return -EINVAL;

	hdr = (const struct rlc_firmware_header_v2_0 *)adev->gfx.rlc_fw->data;
	amdgpu_ucode_print_rlc_hdr(&hdr->header);

	fw_data = (const __le32 *)(adev->gfx.rlc_fw->data +
			   le32_to_cpu(hdr->header.ucode_array_offset_bytes));
	fw_size = le32_to_cpu(hdr->header.ucode_size_bytes) / 4;

	WREG32_SOC15(GC, 0, mmRLC_GPM_UCODE_ADDR,
		     RLCG_UCODE_LOADING_START_ADDRESS);

	for (i = 0; i < fw_size; i++)
		WREG32_SOC15(GC, 0, mmRLC_GPM_UCODE_DATA,
			     le32_to_cpup(fw_data++));

	WREG32_SOC15(GC, 0, mmRLC_GPM_UCODE_ADDR, adev->gfx.rlc_fw_version);

	return 0;
}

static int gfx_v10_0_rlc_resume(struct amdgpu_device *adev)
{
	int r = 0;
	u32 val;

	if (sgpu_no_hw_access != 0)
		return 0;

	adev->gfx.rlc.funcs->stop(adev);

	/* disable CG */
	WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL, 0);

	/* disable PG */
	WREG32_SOC15(GC, 0, mmRLC_PG_CNTL, 0);

	if (adev->firmware.load_type == AMDGPU_FW_LOAD_DIRECT) {
		/* legacy rlc firmware loading */
		r = gfx_v10_0_rlc_load_microcode(adev);
		if (r)
			return r;
	} else if (adev->firmware.load_type == AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO) {
		/* rlc backdoor autoload firmware */
		r = gfx_v10_0_rlc_backdoor_autoload_enable(adev);
		if (r)
			return r;
	}

	if (adev->asic_type == CHIP_VANGOGH_LITE) {
		gfx_v10_0_rlc_enable_srm(adev);

		/* adjust MEM_SLEEP_DELAY per platform */
		val = RREG32(SOC15_REG_OFFSET(GC, 0, mmRLC_PG_DELAY));
		val |= 0x80 << RLC_PG_DELAY__MEM_SLEEP_DELAY__SHIFT;
		WREG32(SOC15_REG_OFFSET(GC, 0, mmRLC_PG_DELAY), val);
	}

	adev->gfx.rlc.funcs->start(adev);

	if (adev->firmware.load_type == AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO) {
		r = gfx_v10_0_wait_for_rlc_autoload_complete(adev);
		if (r)
			return r;
	}

	return 0;
}

static int gfx_v10_0_fw_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int r = 0;

	if (sgpu_no_hw_access != 0)
		return 0;

#ifdef CONFIG_DRM_SGPU_EXYNOS
	/* value will be replaced with Exynos header defined name */
	r = exynos_smc(0x82000520, 0, 0, 0);
	if (r) {
		DRM_ERROR("Failed to load FW by RLC selfloading (%#x)", r);
		return r;
	}

	if (adev->firmware.load_type == AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO) {
		r = gfx_v10_0_wait_for_rlc_autoload_complete(adev);
		if (r)
			return r;
	}

	DRM_DEBUG("DEBUG SCI: ACEM setting.\n");
	if (AMDGPU_IS_MGFX1(adev->grbm_chip_rev) ||
	    AMDGPU_IS_MGFX2_EVT0(adev->grbm_chip_rev)) {
		WREG32_SOC15(GC, 0, mmGL2ACEM_BIN0, 0x124D96DB); // RegWrite GL2ACEM_LLC_MTY0_CTRL0 0x3f140
		WREG32_SOC15(GC, 0, mmGL2ACEM_LLC0_RD, 0x0001B69A); // RegWrite GL2ACEM_LLC_MTY0_CTRL1 0x3f144
		WREG32_SOC15(GC, 0, mmGL2ACEM_LLC_MTY_32DG, 0x00000001); // 32B no allocate for both RD/WR 0x3f188
	} else if (AMDGPU_IS_MGFX2_EVT1(adev->grbm_chip_rev)) {
		writel(0x124D96DB, (void __iomem *)adev->rmmio + 0x31200); // RegWrite  mmACEM_LLC_RD1 0x31200
		writel(0x0001B69A, (void __iomem *)adev->rmmio + 0x31204); // RegWrite  mmACEM_LLC_WR0 0x31204
		writel(0x00000001, (void __iomem *)adev->rmmio + 0x31248); // RegWrite  mmACEM_LLC_WR1 0x31248
	}
	DRM_DEBUG("DEBUG SCI: ACEM setting end.\n");

	/* W/A for RTL fix issue related to read traffic back-pressure and FIFO priority*/
	if (AMDGPU_IS_MGFX2_EVT1(adev->grbm_chip_rev)) {
		WREG32_SOC15(GC, 0, mmTCP_CNTL3, RREG32_SOC15(GC, 0, mmTCP_CNTL3) | 0x4);
		WREG32_SOC15(GC, 0, mmGL2C_CTRL3, 0xD80F40C0);
		WREG32_SOC15(GC, 0, mmGL2C_CTRL4, 0x6006E404);
	} else if(AMDGPU_IS_MGFX1_MID_EVT0(adev->grbm_chip_rev)) {
		WREG32_SOC15(GC, 0, mmGL2C_CTRL3, 0xA00F40C8);
		WREG32_SOC15(GC, 0, mmGL2C_CTRL4, 0x00010404);
	}

	/*
	 * GFXSW-24476: Override the default value of SUBID_QUEUE_MODE_SELECT (0x5) with 0x4.
	 * The default setting makes memory reads sometimes not see memory writes issued by the same wave.
	 */
	if (AMDGPU_IS_MGFX2_EVT1(adev->grbm_chip_rev)) {
		uint32_t reg;
		reg = readl_relaxed(adev->rmmio + 0x31300);	// Read GL2C_CTRL5
		reg &= ~(0x7 << 17);				// Clear SUBID_QUEUE_MODE_SELECT field
		reg |= (0x4 << 17);				// Set SUBID_QUEUE_MODE_SELECT field to 0x4
		writel_relaxed(reg, adev->rmmio + 0x31300);	// Write GL2C_CTRL5
	} else if (AMDGPU_IS_MGFX2_EVT0(adev->grbm_chip_rev)) {
		uint32_t reg;
		reg = readl_relaxed(adev->rmmio + 0x33854);	// Read GL2C_CTRL4
		reg &= ~(0x7 << 17);				// Clear SUBID_QUEUE_MODE_SELECT field
		reg |= (0x4 << 17);				// Set SUBID_QUEUE_MODE_SELECT field to 0x4
		writel_relaxed(reg, adev->rmmio + 0x33854);	// Write GL2C_CTRL4
	}

#else /* CONFIG_DRM_SGPU_EXYNOS */
	if (amdgpu_emu_mode == 1)
		emu_write_rsmu_registers(adev);

	r = gfx_v10_0_rlc_resume(adev);
#endif /* CONFIG_DRM_SGPU_EXYNOS */
	return r;
}

static struct {
	FIRMWARE_ID	id;
	unsigned int	offset;
	unsigned int	size;
} rlc_autoload_info[FIRMWARE_ID_MAX];

#define VANGOGH_LITE_TOC_OFFSET 0x4000

static int gfx_v10_0_parse_rlc_toc(struct amdgpu_device *adev)
{
	RLC_TABLE_OF_CONTENT *rlc_toc;
	uint32_t toc_offset;

	switch (adev->asic_type) {
	case CHIP_VANGOGH_LITE:
		/* TODO: GFXSW-4651 parse TOC offset from header */
		toc_offset = VANGOGH_LITE_TOC_OFFSET;
		break;
	default:
		BUG();
	}

	rlc_toc = (RLC_TABLE_OF_CONTENT *)(adev->gfx.rlc.rlc_autoload_ptr +
					   toc_offset);
	while (rlc_toc && (rlc_toc->id > FIRMWARE_ID_INVALID) &&
		(rlc_toc->id < FIRMWARE_ID_MAX)) {
		if ((adev->asic_type != CHIP_VANGOGH_LITE) &&
		    (rlc_toc->id >= FIRMWARE_ID_CP_CE) &&
		    (rlc_toc->id <= FIRMWARE_ID_CP_MES)) {
			/* Offset needs 4KB alignment */
			rlc_toc->offset = ALIGN(rlc_toc->offset * 4, PAGE_SIZE);
		}

		rlc_autoload_info[rlc_toc->id].id = rlc_toc->id;
		rlc_autoload_info[rlc_toc->id].offset = rlc_toc->offset * 4;
		rlc_autoload_info[rlc_toc->id].size = rlc_toc->size * 4;

		DRM_INFO("Firmware id %d, offset 0x%x, size 0x%x\n",
			rlc_autoload_info[rlc_toc->id].id,
			rlc_autoload_info[rlc_toc->id].offset,
			rlc_autoload_info[rlc_toc->id].size);

		rlc_toc++;
	}

	return 0;
}

static uint32_t gfx_v10_0_calc_toc_total_size(struct amdgpu_device *adev)
{
	uint32_t total_size = 0;
	FIRMWARE_ID id;
	int ret;

	ret = gfx_v10_0_parse_rlc_toc(adev);
	if (ret) {
		dev_err(adev->dev, "failed to parse rlc toc\n");
		return 0;
	}

	if (adev->asic_type == CHIP_VANGOGH_LITE) {
		if (!adev->gfx.unified_fw) {
			dev_err(adev->dev, "unified fw not loaded\n");
			return 0;
		} else {
			return adev->gfx.unified_fw->size;
		}
	}

	for (id = FIRMWARE_ID_RLC_G_UCODE; id < FIRMWARE_ID_MAX; id++)
		total_size += rlc_autoload_info[id].size;

	/* In case the offset in rlc toc ucode is aligned */
	if (total_size < rlc_autoload_info[FIRMWARE_ID_MAX-1].offset)
		total_size = rlc_autoload_info[FIRMWARE_ID_MAX-1].offset +
				rlc_autoload_info[FIRMWARE_ID_MAX-1].size;

	return total_size;
}

static int gfx_v10_0_rlc_backdoor_autoload_buffer_parse(struct amdgpu_device *adev)
{
	const struct firmware *fw = adev->gfx.unified_fw;
	uint32_t *data;

	if (!fw)
		return 0;

	data = (u32*)((u8*)fw->data+SGPU_VERSION_OFFSET);

	adev->gfx.me_fw_version = le32_to_cpu(
		data[GET_U32_OFFSET(ME_VERSION_OFFSET)]);
	adev->gfx.me_feature_version = 0;

	adev->gfx.mec_fw_version = le32_to_cpu(
		data[GET_U32_OFFSET(MEC_VERSION_OFFSET)]);
	adev->gfx.mec_feature_version = 0;

	adev->gfx.pfp_fw_version = le32_to_cpu(
		data[GET_U32_OFFSET(PFP_VERSION_OFFSET)]);
	adev->gfx.pfp_feature_version = 0;

	adev->gfx.rlc_fw_version = le32_to_cpu(
		data[GET_U32_OFFSET(RLC_VERSION_OFFSET)]);
	adev->gfx.rlc_feature_version = 0;

	return 0;
}

static int gfx_v10_0_cl_version_info_parse(struct amdgpu_device *adev)
{
	const struct firmware *fw = adev->gfx.unified_fw;

	if (!fw)
		return 0;

	adev->gfx.sgpu_rtl_cl_number = *(u32*)((u8*)fw->data+CL_VERSION_OFFSET);
	DRM_INFO("amdgpu Perforce RTL CL version: %u\n", adev->gfx.sgpu_rtl_cl_number);

	return 0;
}

static int gfx_v10_0_firmware_version_info_parse(struct amdgpu_device *adev)
{
	const struct firmware *fw = adev->gfx.unified_fw;

	if (!fw)
		return 0;

	adev->gfx.sgpu_fw_major_version = *((u8*)fw->data+SGPU_VERSION_OFFSET+3);
	adev->gfx.sgpu_fw_minor_version = *((u8*)fw->data+SGPU_VERSION_OFFSET+2);
	adev->gfx.sgpu_fw_option_version = *((u8*)fw->data+SGPU_VERSION_OFFSET+1);

	DRM_INFO("amdgpu firmware version: %u.%u.%u\n",
		adev->gfx.sgpu_fw_major_version,
		adev->gfx.sgpu_fw_minor_version,
		adev->gfx.sgpu_fw_option_version);

	return 0;
}

#define SGPU_FIRMWARE_BUFFER_SIZE     0xE0000

static int sgpu_setup_firmware_buffer(struct amdgpu_device *adev)
{
	struct page *page;
	int alloc_order = get_order(SGPU_FIRMWARE_BUFFER_SIZE);
	int i;
	/*
	 * The firmware buffer needs to be 0xe0000 bytes (917,504 = 224 4k pages) that is not
	 * aligned by the power of 2. So first, allocate 0x100000 bytes then free 0x100000 - 0xe0000
	 * bytes.
	 */
	page = alloc_pages(GFP_KERNEL | __GFP_ZERO | GFP_DMA32, alloc_order);
	if (!page)
		return -ENOMEM;

	split_page(page, alloc_order);
	for (i = (SGPU_FIRMWARE_BUFFER_SIZE >> PAGE_SHIFT); i < (1 << alloc_order); i++)
		__free_page(page + i);

	adev->gfx.rlc.rlc_autoload_ptr = page_address(page);
	adev->gfx.rlc.rlc_autoload_gpu_addr = page_to_phys(page);
	adev->gfx.rlc.autoload_size = SGPU_FIRMWARE_BUFFER_SIZE;

	DRM_INFO("firmware memory pa %pa cva 0x%p size %#x",
		 &adev->gfx.rlc.rlc_autoload_gpu_addr,
		 adev->gfx.rlc.rlc_autoload_ptr,
		 adev->gfx.rlc.autoload_size);

	return 0;
}

static int gfx_v10_0_rlc_backdoor_autoload_buffer_init(struct amdgpu_device *adev)
{
	int r = 0;
	uint32_t total_size;

	r = sgpu_setup_firmware_buffer(adev);
	if (r)
		return r;

	sgpu_firmware_clean(adev, 0, adev->gfx.rlc.autoload_size);

	r = sgpu_request_firmware(adev);
	if (r) {
		DRM_ERROR("failed request firmware\n");
		return r;
	}

	total_size = gfx_v10_0_calc_toc_total_size(adev);
	DRM_INFO("amdgpu firmware: %u bytes occupied.\n", total_size);

	gfx_v10_0_rlc_backdoor_autoload_buffer_parse(adev);
	gfx_v10_0_firmware_version_info_parse(adev);
	gfx_v10_0_cl_version_info_parse(adev);

	return 0;
}

static void gfx_v10_0_rlc_backdoor_autoload_buffer_fini(struct amdgpu_device *adev)
{
	struct page *page = phys_to_page(adev->gfx.rlc.rlc_autoload_gpu_addr);
	unsigned int nr_pages = adev->gfx.rlc.autoload_size >> PAGE_SHIFT;
	unsigned int i;

	for (i = 0; i < nr_pages; i++)
		__free_page(page + i);
}

static void gfx_v10_0_rlc_backdoor_autoload_copy_ucode(struct amdgpu_device *adev,
						       FIRMWARE_ID id,
						       const void *fw_data,
						       uint32_t fw_size)
{
	uint32_t toc_offset;
	uint32_t toc_fw_size;
	char *ptr = adev->gfx.rlc.rlc_autoload_ptr;

	if (id <= FIRMWARE_ID_INVALID || id >= FIRMWARE_ID_MAX)
		return;

	toc_offset = rlc_autoload_info[id].offset;
	toc_fw_size = rlc_autoload_info[id].size;

	if (fw_size == 0)
		fw_size = toc_fw_size;

	if (fw_size > toc_fw_size)
		fw_size = toc_fw_size;

	memcpy(ptr + toc_offset, fw_data, fw_size);

	if (fw_size < toc_fw_size)
		memset(ptr + toc_offset + fw_size, 0, toc_fw_size - fw_size);

	sgpu_firmware_clean(adev, toc_offset, fw_size + toc_fw_size - fw_size);
}

static void gfx_v10_0_rlc_backdoor_autoload_copy_toc_ucode(struct amdgpu_device *adev)
{
	void *data;
	uint32_t size;

	data = adev->gfx.rlc.rlc_toc_buf;
	size = rlc_autoload_info[FIRMWARE_ID_RLC_TOC].size;

	gfx_v10_0_rlc_backdoor_autoload_copy_ucode(adev,
						   FIRMWARE_ID_RLC_TOC,
						   data, size);
}

static void gfx_v10_0_rlc_backdoor_autoload_copy_gfx_ucode(struct amdgpu_device *adev)
{
	const __le32 *fw_data;
	uint32_t fw_size;
	const struct gfx_firmware_header_v1_0 *cp_hdr;
	const struct rlc_firmware_header_v2_0 *rlc_hdr;

	/* pfp ucode */
	cp_hdr = (const struct gfx_firmware_header_v1_0 *)
		adev->gfx.pfp_fw->data;
	fw_data = (const __le32 *)(adev->gfx.pfp_fw->data +
		le32_to_cpu(cp_hdr->header.ucode_array_offset_bytes));
	fw_size = le32_to_cpu(cp_hdr->header.ucode_size_bytes);
	gfx_v10_0_rlc_backdoor_autoload_copy_ucode(adev,
						   FIRMWARE_ID_CP_PFP,
						   fw_data, fw_size);

	/* ce ucode */
	cp_hdr = (const struct gfx_firmware_header_v1_0 *)
		adev->gfx.ce_fw->data;
	fw_data = (const __le32 *)(adev->gfx.ce_fw->data +
		le32_to_cpu(cp_hdr->header.ucode_array_offset_bytes));
	fw_size = le32_to_cpu(cp_hdr->header.ucode_size_bytes);
	gfx_v10_0_rlc_backdoor_autoload_copy_ucode(adev,
						   FIRMWARE_ID_CP_CE,
						   fw_data, fw_size);

	/* me ucode */
	cp_hdr = (const struct gfx_firmware_header_v1_0 *)
		adev->gfx.me_fw->data;
	fw_data = (const __le32 *)(adev->gfx.me_fw->data +
		le32_to_cpu(cp_hdr->header.ucode_array_offset_bytes));
	fw_size = le32_to_cpu(cp_hdr->header.ucode_size_bytes);
	gfx_v10_0_rlc_backdoor_autoload_copy_ucode(adev,
						   FIRMWARE_ID_CP_ME,
						   fw_data, fw_size);

	/* rlc ucode */
	rlc_hdr = (const struct rlc_firmware_header_v2_0 *)
		adev->gfx.rlc_fw->data;
	fw_data = (const __le32 *)(adev->gfx.rlc_fw->data +
		le32_to_cpu(rlc_hdr->header.ucode_array_offset_bytes));
	fw_size = le32_to_cpu(rlc_hdr->header.ucode_size_bytes);
	gfx_v10_0_rlc_backdoor_autoload_copy_ucode(adev,
						   FIRMWARE_ID_RLC_G_UCODE,
						   fw_data, fw_size);

	/* mec1 ucode */
	cp_hdr = (const struct gfx_firmware_header_v1_0 *)
		adev->gfx.mec_fw->data;
	fw_data = (const __le32 *) (adev->gfx.mec_fw->data +
		le32_to_cpu(cp_hdr->header.ucode_array_offset_bytes));
	fw_size = le32_to_cpu(cp_hdr->header.ucode_size_bytes) -
		cp_hdr->jt_size * 4;
	gfx_v10_0_rlc_backdoor_autoload_copy_ucode(adev,
						   FIRMWARE_ID_CP_MEC,
						   fw_data, fw_size);
	/* mec2 ucode is not necessary if mec2 ucode is same as mec1 */
}

/* Temporarily put sdma part here */
static void gfx_v10_0_rlc_backdoor_autoload_copy_sdma_ucode(struct amdgpu_device *adev)
{
	const __le32 *fw_data;
	uint32_t fw_size;
	const struct sdma_firmware_header_v1_0 *sdma_hdr;
	int i;

	for (i = 0; i < adev->sdma.num_instances; i++) {
		sdma_hdr = (const struct sdma_firmware_header_v1_0 *)
			adev->sdma.instance[i].fw->data;
		fw_data = (const __le32 *) (adev->sdma.instance[i].fw->data +
			le32_to_cpu(sdma_hdr->header.ucode_array_offset_bytes));
		fw_size = le32_to_cpu(sdma_hdr->header.ucode_size_bytes);

		if (i == 0) {
			gfx_v10_0_rlc_backdoor_autoload_copy_ucode(adev,
				FIRMWARE_ID_SDMA0_UCODE, fw_data, fw_size);
			gfx_v10_0_rlc_backdoor_autoload_copy_ucode(adev,
				FIRMWARE_ID_SDMA0_JT,
				(uint32_t *)fw_data +
				sdma_hdr->jt_offset,
				sdma_hdr->jt_size * 4);
		} else if (i == 1) {
			gfx_v10_0_rlc_backdoor_autoload_copy_ucode(adev,
				FIRMWARE_ID_SDMA1_UCODE, fw_data, fw_size);
			gfx_v10_0_rlc_backdoor_autoload_copy_ucode(adev,
				FIRMWARE_ID_SDMA1_JT,
				(uint32_t *)fw_data +
				sdma_hdr->jt_offset,
				sdma_hdr->jt_size * 4);
		}
	}
}

static int gfx_v10_0_rlc_backdoor_autoload_enable(struct amdgpu_device *adev)
{
	uint32_t rlc_g_offset, rlc_g_size, tmp;
	uint64_t gpu_addr;

	if (adev->asic_type != CHIP_VANGOGH_LITE) {
		gfx_v10_0_rlc_backdoor_autoload_copy_toc_ucode(adev);
		gfx_v10_0_rlc_backdoor_autoload_copy_sdma_ucode(adev);
		gfx_v10_0_rlc_backdoor_autoload_copy_gfx_ucode(adev);
	}

	rlc_g_offset = rlc_autoload_info[FIRMWARE_ID_RLC_G_UCODE].offset;
	rlc_g_size = rlc_autoload_info[FIRMWARE_ID_RLC_G_UCODE].size;
	gpu_addr = adev->gfx.rlc.rlc_autoload_gpu_addr + rlc_g_offset;

	WREG32_SOC15(GC, 0, mmRLC_HYP_BOOTLOAD_ADDR_HI, upper_32_bits(gpu_addr));
	WREG32_SOC15(GC, 0, mmRLC_HYP_BOOTLOAD_ADDR_LO, lower_32_bits(gpu_addr));
	WREG32_SOC15(GC, 0, mmRLC_HYP_BOOTLOAD_SIZE, rlc_g_size);

	tmp = RREG32_SOC15(GC, 0, mmRLC_HYP_RESET_VECTOR);
	if (!(tmp & (RLC_HYP_RESET_VECTOR__COLD_BOOT_EXIT_MASK |
		   RLC_HYP_RESET_VECTOR__VDDGFX_EXIT_MASK))) {
		DRM_ERROR("Neither COLD_BOOT_EXIT nor VDDGFX_EXIT is set\n");
		return -EINVAL;
	}

	tmp = RREG32_SOC15(GC, 0, mmRLC_CNTL);
	if (tmp & RLC_CNTL__RLC_ENABLE_F32_MASK) {
		DRM_ERROR("RLC ROM should halt itself\n");
		return -EINVAL;
	}

	return 0;
}

static int gfx_v10_0_wait_for_rlc_autoload_complete(struct amdgpu_device *adev)
{
	uint32_t cp_status;
	uint32_t bootload_status;
	int i;

	for (i = 0; i < adev->usec_timeout; i++) {
		cp_status = RREG32_SOC15(GC, 0, mmCP_STAT);
		bootload_status = RREG32_SOC15(GC, 0, mmRLC_RLCS_BOOTLOAD_STATUS);
		if ((cp_status == 0) &&
		    (REG_GET_FIELD(bootload_status,
			RLC_RLCS_BOOTLOAD_STATUS, BOOTLOAD_COMPLETE) == 1)) {
			break;
		}
		if (amdgpu_emu_mode) {
			if (i >= 1000)
				break;
			msleep(1);
		} else {
			udelay(1);
		}
	}
	SGPU_LOG(adev, DMSG_INFO, DMSG_POWER,
		"cp_status:0x%x, bootload_status:0x%x", cp_status, bootload_status);


	if (i >= adev->usec_timeout) {
		dev_err(adev->dev, "HW defect detected : "
			"rlc autoload: gc ucode autoload timeout. fw init failed\n");
		sgpu_debug_snapshot_expire_watchdog();
		return -ETIMEDOUT;
	}

	return 0;
}

static int gfx_v10_0_cp_gfx_enable(struct amdgpu_device *adev, bool enable)
{
	int i;
	u32 tmp = RREG32_SOC15(GC, 0, mmCP_ME_CNTL);

	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, enable ? 0 : 1);

	if (adev->asic_type != CHIP_VANGOGH_LITE)
		tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, CE_HALT, enable ? 0 : 1);

	WREG32_SOC15(GC, 0, mmCP_ME_CNTL, tmp);

	if (adev->job_hang && !enable)
		return 0;

	if (amdgpu_in_reset(adev) && !enable)
		return 0;

	if (adev->job_hang && !enable)
		return 0;

	for (i = 0; i < adev->usec_timeout; i++) {
		if (RREG32_SOC15(GC, 0, mmCP_STAT) == 0)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout)
		DRM_ERROR("failed to %s cp gfx\n", enable ? "unhalt" : "halt");

	return 0;
}

static int gfx_v10_0_cp_gfx_load_pfp_microcode(struct amdgpu_device *adev)
{
	int r;
	const struct gfx_firmware_header_v1_0 *pfp_hdr;
	const __le32 *fw_data;
	unsigned i, fw_size;
	uint32_t tmp;
	uint32_t usec_timeout = 50000;  /* wait for 50ms */

	pfp_hdr = (const struct gfx_firmware_header_v1_0 *)
		adev->gfx.pfp_fw->data;

	amdgpu_ucode_print_gfx_hdr(&pfp_hdr->header);

	fw_data = (const __le32 *)(adev->gfx.pfp_fw->data +
		le32_to_cpu(pfp_hdr->header.ucode_array_offset_bytes));
	fw_size = le32_to_cpu(pfp_hdr->header.ucode_size_bytes);

	r = amdgpu_bo_create_reserved(adev, pfp_hdr->header.ucode_size_bytes,
				      PAGE_SIZE, AMDGPU_GEM_DOMAIN_GTT,
				      &adev->gfx.pfp.pfp_fw_obj,
				      &adev->gfx.pfp.pfp_fw_gpu_addr,
				      (void **)&adev->gfx.pfp.pfp_fw_ptr);
	if (r) {
		dev_err(adev->dev, "(%d) failed to create pfp fw bo\n", r);
		gfx_v10_0_pfp_fini(adev);
		return r;
	}

	memcpy(adev->gfx.pfp.pfp_fw_ptr, fw_data, fw_size);

	amdgpu_bo_kunmap(adev->gfx.pfp.pfp_fw_obj);
	amdgpu_bo_unreserve(adev->gfx.pfp.pfp_fw_obj);

	/* Trigger an invalidation of the L1 instruction caches */
	tmp = RREG32_SOC15(GC, 0, mmCP_PFP_IC_OP_CNTL);
	tmp = REG_SET_FIELD(tmp, CP_PFP_IC_OP_CNTL, INVALIDATE_CACHE, 1);
	WREG32_SOC15(GC, 0, mmCP_PFP_IC_OP_CNTL, tmp);

	/* Wait for invalidation complete */
	for (i = 0; i < usec_timeout; i++) {
		tmp = RREG32_SOC15(GC, 0, mmCP_PFP_IC_OP_CNTL);
		if (1 == REG_GET_FIELD(tmp, CP_PFP_IC_OP_CNTL,
			INVALIDATE_CACHE_COMPLETE))
			break;
		udelay(1);
	}

	if (i >= usec_timeout) {
		dev_err(adev->dev, "failed to invalidate instruction cache\n");
		return -EINVAL;
	}

	if (amdgpu_emu_mode == 1)
		adev->nbio.funcs->hdp_flush(adev, NULL);

	tmp = RREG32_SOC15(GC, 0, mmCP_PFP_IC_BASE_CNTL);
	tmp = REG_SET_FIELD(tmp, CP_PFP_IC_BASE_CNTL, VMID, 0);
	tmp = REG_SET_FIELD(tmp, CP_PFP_IC_BASE_CNTL, CACHE_POLICY, 0);
	tmp = REG_SET_FIELD(tmp, CP_PFP_IC_BASE_CNTL, EXE_DISABLE, 0);
	tmp = REG_SET_FIELD(tmp, CP_PFP_IC_BASE_CNTL, ADDRESS_CLAMP, 1);
	WREG32_SOC15(GC, 0, mmCP_PFP_IC_BASE_CNTL, tmp);
	WREG32_SOC15(GC, 0, mmCP_PFP_IC_BASE_LO,
		adev->gfx.pfp.pfp_fw_gpu_addr & 0xFFFFF000);
	WREG32_SOC15(GC, 0, mmCP_PFP_IC_BASE_HI,
		upper_32_bits(adev->gfx.pfp.pfp_fw_gpu_addr));

	WREG32_SOC15(GC, 0, mmCP_HYP_PFP_UCODE_ADDR, 0);

	for (i = 0; i < pfp_hdr->jt_size; i++)
		WREG32_SOC15(GC, 0, mmCP_HYP_PFP_UCODE_DATA,
			     le32_to_cpup(fw_data + pfp_hdr->jt_offset + i));

	WREG32_SOC15(GC, 0, mmCP_HYP_PFP_UCODE_ADDR, adev->gfx.pfp_fw_version);

	return 0;
}

static int gfx_v10_0_cp_gfx_load_ce_microcode(struct amdgpu_device *adev)
{
	int r;
	const struct gfx_firmware_header_v1_0 *ce_hdr;
	const __le32 *fw_data;
	unsigned i, fw_size;
	uint32_t tmp;
	uint32_t usec_timeout = 50000;  /* wait for 50ms */

	ce_hdr = (const struct gfx_firmware_header_v1_0 *)
		adev->gfx.ce_fw->data;

	amdgpu_ucode_print_gfx_hdr(&ce_hdr->header);

	fw_data = (const __le32 *)(adev->gfx.ce_fw->data +
		le32_to_cpu(ce_hdr->header.ucode_array_offset_bytes));
	fw_size = le32_to_cpu(ce_hdr->header.ucode_size_bytes);

	r = amdgpu_bo_create_reserved(adev, ce_hdr->header.ucode_size_bytes,
				      PAGE_SIZE, AMDGPU_GEM_DOMAIN_GTT,
				      &adev->gfx.ce.ce_fw_obj,
				      &adev->gfx.ce.ce_fw_gpu_addr,
				      (void **)&adev->gfx.ce.ce_fw_ptr);
	if (r) {
		dev_err(adev->dev, "(%d) failed to create ce fw bo\n", r);
		gfx_v10_0_ce_fini(adev);
		return r;
	}

	memcpy(adev->gfx.ce.ce_fw_ptr, fw_data, fw_size);

	amdgpu_bo_kunmap(adev->gfx.ce.ce_fw_obj);
	amdgpu_bo_unreserve(adev->gfx.ce.ce_fw_obj);

	/* Trigger an invalidation of the L1 instruction caches */
	tmp = RREG32_SOC15(GC, 0, mmCP_CE_IC_OP_CNTL);
	tmp = REG_SET_FIELD(tmp, CP_CE_IC_OP_CNTL, INVALIDATE_CACHE, 1);
	WREG32_SOC15(GC, 0, mmCP_CE_IC_OP_CNTL, tmp);

	/* Wait for invalidation complete */
	for (i = 0; i < usec_timeout; i++) {
		tmp = RREG32_SOC15(GC, 0, mmCP_CE_IC_OP_CNTL);
		if (1 == REG_GET_FIELD(tmp, CP_CE_IC_OP_CNTL,
			INVALIDATE_CACHE_COMPLETE))
			break;
		udelay(1);
	}

	if (i >= usec_timeout) {
		dev_err(adev->dev, "failed to invalidate instruction cache\n");
		return -EINVAL;
	}

	if (amdgpu_emu_mode == 1)
		adev->nbio.funcs->hdp_flush(adev, NULL);

	tmp = RREG32_SOC15(GC, 0, mmCP_CE_IC_BASE_CNTL);
	tmp = REG_SET_FIELD(tmp, CP_CE_IC_BASE_CNTL, VMID, 0);
	tmp = REG_SET_FIELD(tmp, CP_CE_IC_BASE_CNTL, CACHE_POLICY, 0);
	tmp = REG_SET_FIELD(tmp, CP_CE_IC_BASE_CNTL, EXE_DISABLE, 0);
	tmp = REG_SET_FIELD(tmp, CP_CE_IC_BASE_CNTL, ADDRESS_CLAMP, 1);
	WREG32_SOC15(GC, 0, mmCP_CE_IC_BASE_LO,
		adev->gfx.ce.ce_fw_gpu_addr & 0xFFFFF000);
	WREG32_SOC15(GC, 0, mmCP_CE_IC_BASE_HI,
		upper_32_bits(adev->gfx.ce.ce_fw_gpu_addr));

	WREG32_SOC15(GC, 0, mmCP_HYP_CE_UCODE_ADDR, 0);

	for (i = 0; i < ce_hdr->jt_size; i++)
		WREG32_SOC15(GC, 0, mmCP_HYP_CE_UCODE_DATA,
			     le32_to_cpup(fw_data + ce_hdr->jt_offset + i));

	WREG32_SOC15(GC, 0, mmCP_HYP_CE_UCODE_ADDR, adev->gfx.ce_fw_version);

	return 0;
}

static int gfx_v10_0_cp_gfx_load_me_microcode(struct amdgpu_device *adev)
{
	int r;
	const struct gfx_firmware_header_v1_0 *me_hdr;
	const __le32 *fw_data;
	unsigned i, fw_size;
	uint32_t tmp;
	uint32_t usec_timeout = 50000;  /* wait for 50ms */

	me_hdr = (const struct gfx_firmware_header_v1_0 *)
		adev->gfx.me_fw->data;

	amdgpu_ucode_print_gfx_hdr(&me_hdr->header);

	fw_data = (const __le32 *)(adev->gfx.me_fw->data +
		le32_to_cpu(me_hdr->header.ucode_array_offset_bytes));
	fw_size = le32_to_cpu(me_hdr->header.ucode_size_bytes);

	r = amdgpu_bo_create_reserved(adev, me_hdr->header.ucode_size_bytes,
				      PAGE_SIZE, AMDGPU_GEM_DOMAIN_GTT,
				      &adev->gfx.me.me_fw_obj,
				      &adev->gfx.me.me_fw_gpu_addr,
				      (void **)&adev->gfx.me.me_fw_ptr);
	if (r) {
		dev_err(adev->dev, "(%d) failed to create me fw bo\n", r);
		gfx_v10_0_me_fini(adev);
		return r;
	}

	memcpy(adev->gfx.me.me_fw_ptr, fw_data, fw_size);

	amdgpu_bo_kunmap(adev->gfx.me.me_fw_obj);
	amdgpu_bo_unreserve(adev->gfx.me.me_fw_obj);

	/* Trigger an invalidation of the L1 instruction caches */
	tmp = RREG32_SOC15(GC, 0, mmCP_ME_IC_OP_CNTL);
	tmp = REG_SET_FIELD(tmp, CP_ME_IC_OP_CNTL, INVALIDATE_CACHE, 1);
	WREG32_SOC15(GC, 0, mmCP_ME_IC_OP_CNTL, tmp);

	/* Wait for invalidation complete */
	for (i = 0; i < usec_timeout; i++) {
		tmp = RREG32_SOC15(GC, 0, mmCP_ME_IC_OP_CNTL);
		if (1 == REG_GET_FIELD(tmp, CP_ME_IC_OP_CNTL,
			INVALIDATE_CACHE_COMPLETE))
			break;
		udelay(1);
	}

	if (i >= usec_timeout) {
		dev_err(adev->dev, "failed to invalidate instruction cache\n");
		return -EINVAL;
	}

	if (amdgpu_emu_mode == 1)
		adev->nbio.funcs->hdp_flush(adev, NULL);

	tmp = RREG32_SOC15(GC, 0, mmCP_ME_IC_BASE_CNTL);
	tmp = REG_SET_FIELD(tmp, CP_ME_IC_BASE_CNTL, VMID, 0);
	tmp = REG_SET_FIELD(tmp, CP_ME_IC_BASE_CNTL, CACHE_POLICY, 0);
	tmp = REG_SET_FIELD(tmp, CP_ME_IC_BASE_CNTL, EXE_DISABLE, 0);
	tmp = REG_SET_FIELD(tmp, CP_ME_IC_BASE_CNTL, ADDRESS_CLAMP, 1);
	WREG32_SOC15(GC, 0, mmCP_ME_IC_BASE_LO,
		adev->gfx.me.me_fw_gpu_addr & 0xFFFFF000);
	WREG32_SOC15(GC, 0, mmCP_ME_IC_BASE_HI,
		upper_32_bits(adev->gfx.me.me_fw_gpu_addr));

	WREG32_SOC15(GC, 0, mmCP_HYP_ME_UCODE_ADDR, 0);

	for (i = 0; i < me_hdr->jt_size; i++)
		WREG32_SOC15(GC, 0, mmCP_HYP_ME_UCODE_DATA,
			     le32_to_cpup(fw_data + me_hdr->jt_offset + i));

	WREG32_SOC15(GC, 0, mmCP_HYP_ME_UCODE_ADDR, adev->gfx.me_fw_version);

	return 0;
}

static int gfx_v10_0_cp_gfx_load_microcode(struct amdgpu_device *adev)
{
	int r;

	if (!adev->gfx.me_fw || !adev->gfx.pfp_fw || !adev->gfx.ce_fw) {
		if ((adev->asic_type == CHIP_VANGOGH_LITE) &&
			(!adev->gfx.ce_fw))
			DRM_INFO(" No need to check ce fw for VANGOGH_LITE!\n");
		else
			return -EINVAL;
	}

	gfx_v10_0_cp_gfx_enable(adev, false);

	r = gfx_v10_0_cp_gfx_load_pfp_microcode(adev);
	if (r) {
		dev_err(adev->dev, "(%d) failed to load pfp fw\n", r);
		return r;
	}

	if (adev->asic_type != CHIP_VANGOGH_LITE) {
		r = gfx_v10_0_cp_gfx_load_ce_microcode(adev);
		if (r) {
			dev_err(adev->dev, "(%d) failed to load ce fw\n", r);
			return r;
		}
	} else
		DRM_INFO("Skip gfx_v10_0_cp_gfx_load_ce_microcode!\n");

	r = gfx_v10_0_cp_gfx_load_me_microcode(adev);
	if (r) {
		dev_err(adev->dev, "(%d) failed to load me fw\n", r);
		return r;
	}

	return 0;
}

static int gfx_v10_0_cp_gfx_start(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;
	int r, i;
	int ctx_reg_offset;

	/* init the CP */
	WREG32_SOC15(GC, 0, mmCP_MAX_CONTEXT,
		     adev->gfx.config.max_hw_contexts - 1);
	WREG32_SOC15(GC, 0, mmCP_DEVICE_ID, 1);

	gfx_v10_0_cp_gfx_enable(adev, true);

	ring = &adev->gfx.gfx_ring[0];
	r = amdgpu_ring_alloc(ring, gfx_v10_0_get_csb_size(adev) + 4);
	if (r) {
		DRM_ERROR("amdgpu: cp failed to lock ring (%d).\n", r);
		return r;
	}

	amdgpu_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	amdgpu_ring_write(ring, PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);

	amdgpu_ring_write(ring, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	amdgpu_ring_write(ring, 0x80000000);
	amdgpu_ring_write(ring, 0x80000000);

	for (sect = gfx10_cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT) {
				amdgpu_ring_write(ring,
						  PACKET3(PACKET3_SET_CONTEXT_REG,
							  ext->reg_count));
				amdgpu_ring_write(ring, ext->reg_index -
						  PACKET3_SET_CONTEXT_REG_START);
				for (i = 0; i < ext->reg_count; i++)
					amdgpu_ring_write(ring, ext->extent[i]);
			}
		}
	}

	ctx_reg_offset =
		SOC15_REG_OFFSET(GC, 0, mmPA_SC_TILE_STEERING_OVERRIDE) - PACKET3_SET_CONTEXT_REG_START;
	amdgpu_ring_write(ring, PACKET3(PACKET3_SET_CONTEXT_REG, 1));
	amdgpu_ring_write(ring, ctx_reg_offset);
	amdgpu_ring_write(ring, adev->gfx.config.pa_sc_tile_steering_override);

	amdgpu_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	amdgpu_ring_write(ring, PACKET3_PREAMBLE_END_CLEAR_STATE);

	amdgpu_ring_write(ring, PACKET3(PACKET3_CLEAR_STATE, 0));
	amdgpu_ring_write(ring, 0);

	amdgpu_ring_write(ring, PACKET3(PACKET3_SET_BASE, 2));
	amdgpu_ring_write(ring, PACKET3_BASE_INDEX(CE_PARTITION_BASE));
	amdgpu_ring_write(ring, 0x8000);
	amdgpu_ring_write(ring, 0x8000);

	amdgpu_ring_commit(ring);

	/* submit cs packet to copy state 0 to next available state */
	if (adev->gfx.num_gfx_rings > 1) {
		/* maximum supported gfx ring is 2 */
		ring = &adev->gfx.gfx_ring[1];
		r = amdgpu_ring_alloc(ring, 2);
		if (r) {
			DRM_ERROR("amdgpu: cp failed to lock ring (%d).\n", r);
			return r;
		}

		amdgpu_ring_write(ring, PACKET3(PACKET3_CLEAR_STATE, 0));
		amdgpu_ring_write(ring, 0);

		amdgpu_ring_commit(ring);
	}
	return 0;
}

static void gfx_v10_0_cp_gfx_switch_pipe(struct amdgpu_device *adev,
					 CP_PIPE_ID pipe)
{
	u32 tmp;

	tmp = RREG32_SOC15(GC, 0, mmGRBM_GFX_CNTL);
	tmp = REG_SET_FIELD(tmp, GRBM_GFX_CNTL, PIPEID, pipe);

	WREG32_SOC15(GC, 0, mmGRBM_GFX_CNTL, tmp);
}

static void gfx_v10_0_cp_gfx_set_doorbell(struct amdgpu_device *adev,
					  struct amdgpu_ring *ring)
{
	u32 tmp;

	tmp = RREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_CONTROL);
	if (ring->use_doorbell) {
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL,
				    DOORBELL_OFFSET, ring->doorbell_index);
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL,
				    DOORBELL_EN, 1);
	} else {
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL,
				    DOORBELL_EN, 0);
	}
	WREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_CONTROL, tmp);
	tmp = REG_SET_FIELD(0, CP_RB_DOORBELL_RANGE_LOWER,
			    DOORBELL_RANGE_LOWER, ring->doorbell_index);
	WREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_RANGE_LOWER, tmp);

	WREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_RANGE_UPPER,
		     CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK);
}

static int gfx_v10_0_cp_gfx_resume(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring;
	u32 tmp;
	u32 rb_bufsz;
	u64 rb_addr, rptr_addr, wptr_gpu_addr;
	u32 i;

	/* Set the write pointer delay */
	WREG32_SOC15(GC, 0, mmCP_RB_WPTR_DELAY, 0);

	/* set the RB to use vmid 0 */
	WREG32_SOC15(GC, 0, mmCP_RB_VMID, 0);

	/* Init gfx ring 0 for pipe 0 */
	mutex_lock(&adev->srbm_mutex);
	gfx_v10_0_cp_gfx_switch_pipe(adev, PIPE_ID0);

	/* Set ring buffer size */
	ring = &adev->gfx.gfx_ring[0];
	rb_bufsz = order_base_2(ring->ring_size / 8);
	tmp = REG_SET_FIELD(0, CP_RB0_CNTL, RB_BUFSZ, rb_bufsz);
	tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BLKSZ, rb_bufsz - 2);
#ifdef __BIG_ENDIAN
	tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, BUF_SWAP, 1);
#endif
	WREG32_SOC15(GC, 0, mmCP_RB0_CNTL, tmp);

	/* Initialize the ring buffer's write pointers */
	ring->wptr = 0;
	WREG32_SOC15(GC, 0, mmCP_RB0_WPTR, lower_32_bits(ring->wptr));
	WREG32_SOC15(GC, 0, mmCP_RB0_WPTR_HI, upper_32_bits(ring->wptr));

	/* set the wb address wether it's enabled or not */
	rptr_addr = adev->wb.gpu_addr + (ring->rptr_offs * 4);
	WREG32_SOC15(GC, 0, mmCP_RB0_RPTR_ADDR, lower_32_bits(rptr_addr));
	WREG32_SOC15(GC, 0, mmCP_RB0_RPTR_ADDR_HI, upper_32_bits(rptr_addr) &
		     CP_RB_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK);

	wptr_gpu_addr = adev->wb.gpu_addr + (ring->wptr_offs * 4);
	WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_ADDR_LO,
		     lower_32_bits(wptr_gpu_addr));
	WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_ADDR_HI,
		     upper_32_bits(wptr_gpu_addr));

	mdelay(1);
	WREG32_SOC15(GC, 0, mmCP_RB0_CNTL, tmp);

	rb_addr = ring->gpu_addr >> 8;
	WREG32_SOC15(GC, 0, mmCP_RB0_BASE, rb_addr);
	WREG32_SOC15(GC, 0, mmCP_RB0_BASE_HI, upper_32_bits(rb_addr));

	WREG32_SOC15(GC, 0, mmCP_RB_ACTIVE, 1);

	gfx_v10_0_cp_gfx_set_doorbell(adev, ring);
	mutex_unlock(&adev->srbm_mutex);

	/* Init gfx ring 1 for pipe 1 */
	if (adev->gfx.num_gfx_rings > 1) {
		mutex_lock(&adev->srbm_mutex);
		gfx_v10_0_cp_gfx_switch_pipe(adev, PIPE_ID1);
		/* maximum supported gfx ring is 2 */
		ring = &adev->gfx.gfx_ring[1];
		rb_bufsz = order_base_2(ring->ring_size / 8);
		tmp = REG_SET_FIELD(0, CP_RB1_CNTL, RB_BUFSZ, rb_bufsz);
		tmp = REG_SET_FIELD(tmp, CP_RB1_CNTL, RB_BLKSZ, rb_bufsz - 2);
		WREG32_SOC15(GC, 0, mmCP_RB1_CNTL, tmp);
		/* Initialize the ring buffer's write pointers */
		ring->wptr = 0;
		WREG32_SOC15(GC, 0, mmCP_RB1_WPTR, lower_32_bits(ring->wptr));
		WREG32_SOC15(GC, 0, mmCP_RB1_WPTR_HI, upper_32_bits(ring->wptr));
		/* Set the wb address wether it's enabled or not */
		rptr_addr = adev->wb.gpu_addr + (ring->rptr_offs * 4);
		WREG32_SOC15(GC, 0, mmCP_RB1_RPTR_ADDR, lower_32_bits(rptr_addr));
		WREG32_SOC15(GC, 0, mmCP_RB1_RPTR_ADDR_HI, upper_32_bits(rptr_addr) &
			     CP_RB1_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK);
		wptr_gpu_addr = adev->wb.gpu_addr + (ring->wptr_offs * 4);
		WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_ADDR_LO,
			     lower_32_bits(wptr_gpu_addr));
		WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_ADDR_HI,
			     upper_32_bits(wptr_gpu_addr));

		mdelay(1);
		WREG32_SOC15(GC, 0, mmCP_RB1_CNTL, tmp);

		rb_addr = ring->gpu_addr >> 8;
		WREG32_SOC15(GC, 0, mmCP_RB1_BASE, rb_addr);
		WREG32_SOC15(GC, 0, mmCP_RB1_BASE_HI, upper_32_bits(rb_addr));
		WREG32_SOC15(GC, 0, mmCP_RB1_ACTIVE, 1);

		gfx_v10_0_cp_gfx_set_doorbell(adev, ring);
		mutex_unlock(&adev->srbm_mutex);
	}
	/* Switch to pipe 0 */
	mutex_lock(&adev->srbm_mutex);
	gfx_v10_0_cp_gfx_switch_pipe(adev, PIPE_ID0);
	mutex_unlock(&adev->srbm_mutex);

	/* start the ring */
	gfx_v10_0_cp_gfx_start(adev);

	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		ring = &adev->gfx.gfx_ring[i];
		ring->sched.ready = true;
	}

	return 0;
}

static void gfx_v10_0_cp_compute_enable(struct amdgpu_device *adev, bool enable)
{
	if (enable) {
		WREG32_SOC15(GC, 0, mmCP_MEC_CNTL_Vgh_Lite, 0);
	} else {
		WREG32_SOC15(GC, 0, mmCP_MEC_CNTL_Vgh_Lite,
			     (CP_MEC_CNTL__MEC_ME1_HALT_MASK |
			      CP_MEC_CNTL__MEC_ME2_HALT_MASK));
		adev->gfx.kiq.ring.sched.ready = false;
	}
	udelay(50);
}

static int gfx_v10_0_cp_compute_load_microcode(struct amdgpu_device *adev)
{
	const struct gfx_firmware_header_v1_0 *mec_hdr;
	const __le32 *fw_data;
	unsigned i;
	u32 tmp;
	u32 usec_timeout = 50000; /* Wait for 50 ms */

	if (!adev->gfx.mec_fw)
		return -EINVAL;

	gfx_v10_0_cp_compute_enable(adev, false);

	mec_hdr = (const struct gfx_firmware_header_v1_0 *)adev->gfx.mec_fw->data;
	amdgpu_ucode_print_gfx_hdr(&mec_hdr->header);

	fw_data = (const __le32 *)
		(adev->gfx.mec_fw->data +
		 le32_to_cpu(mec_hdr->header.ucode_array_offset_bytes));

	/* Trigger an invalidation of the L1 instruction caches */
	tmp = RREG32_SOC15(GC, 0, mmCP_CPC_IC_OP_CNTL);
	tmp = REG_SET_FIELD(tmp, CP_CPC_IC_OP_CNTL, INVALIDATE_CACHE, 1);
	WREG32_SOC15(GC, 0, mmCP_CPC_IC_OP_CNTL, tmp);

	/* Wait for invalidation complete */
	for (i = 0; i < usec_timeout; i++) {
		tmp = RREG32_SOC15(GC, 0, mmCP_CPC_IC_OP_CNTL);
		if (1 == REG_GET_FIELD(tmp, CP_CPC_IC_OP_CNTL,
				       INVALIDATE_CACHE_COMPLETE))
			break;
		udelay(1);
	}

	if (i >= usec_timeout) {
		dev_err(adev->dev, "failed to invalidate instruction cache\n");
		return -EINVAL;
	}

	if (amdgpu_emu_mode == 1)
		adev->nbio.funcs->hdp_flush(adev, NULL);

	tmp = RREG32_SOC15(GC, 0, mmCP_CPC_IC_BASE_CNTL);
	tmp = REG_SET_FIELD(tmp, CP_CPC_IC_BASE_CNTL, CACHE_POLICY, 0);
	tmp = REG_SET_FIELD(tmp, CP_CPC_IC_BASE_CNTL, EXE_DISABLE, 0);
	tmp = REG_SET_FIELD(tmp, CP_CPC_IC_BASE_CNTL, ADDRESS_CLAMP, 1);
	WREG32_SOC15(GC, 0, mmCP_CPC_IC_BASE_CNTL, tmp);

	WREG32_SOC15(GC, 0, mmCP_CPC_IC_BASE_LO, adev->gfx.mec.mec_fw_gpu_addr &
		     0xFFFFF000);
	WREG32_SOC15(GC, 0, mmCP_CPC_IC_BASE_HI,
		     upper_32_bits(adev->gfx.mec.mec_fw_gpu_addr));

	/* MEC1 */
	WREG32_SOC15(GC, 0, mmCP_MEC_ME1_UCODE_ADDR, 0);

	for (i = 0; i < mec_hdr->jt_size; i++)
		WREG32_SOC15(GC, 0, mmCP_MEC_ME1_UCODE_DATA,
			     le32_to_cpup(fw_data + mec_hdr->jt_offset + i));

	WREG32_SOC15(GC, 0, mmCP_MEC_ME1_UCODE_ADDR, adev->gfx.mec_fw_version);

	/*
	 * TODO: Loading MEC2 firmware is only necessary if MEC2 should run
	 * different microcode than MEC1.
	 */

	return 0;
}

static void gfx_v10_0_kiq_setting(struct amdgpu_ring *ring)
{
	uint32_t tmp;
	struct amdgpu_device *adev = ring->adev;

	/* tell RLC which is KIQ queue */
	tmp = RREG32_SOC15(GC, 0, mmRLC_CP_SCHEDULERS);
	tmp &= 0xffffff00;
	tmp |= (ring->me << 5) | (ring->pipe << 3) | (ring->queue);
	WREG32_SOC15(GC, 0, mmRLC_CP_SCHEDULERS, tmp);
	tmp |= 0x80;
	WREG32_SOC15(GC, 0, mmRLC_CP_SCHEDULERS, tmp);
}

static int gfx_v10_0_gfx_mqd_init_mcbp(struct amdgpu_ring *ring)
{
	struct v10_gfx_mqd *mqd = ring->mqd_ptr;
	struct amdgpu_device *adev = ring->adev;
	uint32_t priority = RREG32_SOC15(GC, 0, mmCP_GFX_HQD_QUEUE_PRIORITY);
	uint32_t quantum = RREG32_SOC15(GC, 0, mmCP_GFX_HQD_QUANTUM);

	switch (amdgpu_mcbp) {
	case 1:
		/* Configure priority based queue switching  */
		/* Set queue 0 as high priority */
		mutex_lock(&ring->priority_mutex);
		if (ring->priority == DRM_SCHED_PRIORITY_HIGH)
			priority = REG_SET_FIELD(priority,
					CP_GFX_HQD_QUEUE_PRIORITY,
					PRIORITY_LEVEL, AMDGPU_GFX_RING_PRIO_HIGH);
		else
			priority = REG_SET_FIELD(priority,
					CP_GFX_HQD_QUEUE_PRIORITY,
					PRIORITY_LEVEL, AMDGPU_GFX_RING_PRIO_LOW);
		mutex_unlock(&ring->priority_mutex);

		/**
		 * Enable and configure Quantum time based HW Queue Switching
 		 * for the HW queues with same priority
 		 */
		quantum = REG_SET_FIELD(quantum, CP_GFX_HQD_QUANTUM,
				QUANTUM_EN, 1);

		/**
		 * QUANTUM_SCALE possible values as per reg spec
		 *    Units used for the quantum countdown counter:
		 *        0 = XXus (~1000 clks)
		 *        1 = XXus (~10K clks)
		 *        2 = 50 us (~100K clks)
		 */
		quantum = REG_SET_FIELD(quantum, CP_GFX_HQD_QUANTUM,
				QUANTUM_SCALE, 0);

		/**
		 * QUANTUM_DURATION is specified in units of QUANTUM_SCALE
		 * Setting QUANTUM_SCALE to 0 and QUANTUM_DURATION as 0xA
		 * would let QueueManager leave the queue attached to pipeline
		 * for 0xA times ~1000 clks
		 */
		quantum = REG_SET_FIELD(quantum, CP_GFX_HQD_QUANTUM,
				QUANTUM_DURATION, (0xFF - 1));

		break;
	case 2:
		priority = REG_SET_FIELD(priority,
				CP_GFX_HQD_QUEUE_PRIORITY,
				PRIORITY_LEVEL, 0);

		quantum = REG_SET_FIELD(quantum, CP_GFX_HQD_QUANTUM,
				QUANTUM_EN, 1);

		quantum = REG_SET_FIELD(quantum, CP_GFX_HQD_QUANTUM,
				QUANTUM_SCALE, 0);

		/* Configure quantum timer based queue switching */
		/* Set a small quantum time for Queue 0 , [15:8] */
		if (ring->me == 0 && ring->pipe == 0 && ring->queue == 0)
			quantum = REG_SET_FIELD(quantum, CP_GFX_HQD_QUANTUM,
					QUANTUM_DURATION, 1);
		else
			quantum = REG_SET_FIELD(quantum, CP_GFX_HQD_QUANTUM,
					QUANTUM_DURATION, (0xFF - 1));
		break;
	default:
		/* Normal setting */
		priority = REG_SET_FIELD(priority,
				CP_GFX_HQD_QUEUE_PRIORITY,
				PRIORITY_LEVEL, 1);
		quantum = REG_SET_FIELD(quantum, CP_GFX_HQD_QUANTUM,
				QUANTUM_EN, 1);
		break;
	}

	mqd->cp_gfx_hqd_queue_priority = priority;
	mqd->cp_gfx_hqd_quantum= quantum;

	return 0;
}

static int gfx_v10_0_gfx_mqd_init(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_gfx_mqd *mqd = ring->mqd_ptr;
	uint64_t hqd_gpu_addr, wb_gpu_addr;
	uint32_t tmp;
	uint32_t rb_bufsz;

	/* set up gfx hqd wptr */
	mqd->cp_gfx_hqd_wptr = 0;
	mqd->cp_gfx_hqd_wptr_hi = 0;

	/* set the pointer to the MQD */
	mqd->cp_mqd_base_addr = ring->mqd_gpu_addr & 0xfffffffc;
	mqd->cp_mqd_base_addr_hi = upper_32_bits(ring->mqd_gpu_addr);

	/* set up mqd control */
	tmp = RREG32_SOC15(GC, 0, mmCP_GFX_MQD_CONTROL);
	tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, VMID, 0);
	tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, PRIV_STATE, 1);
	tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, CACHE_POLICY, 0);
	mqd->cp_gfx_mqd_control = tmp;

	/* set up gfx_hqd_vimd with 0x0 to indicate the ring buffer's vmid */
	tmp = RREG32_SOC15(GC, 0, mmCP_GFX_HQD_VMID);
	tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_VMID, VMID, 0);
	mqd->cp_gfx_hqd_vmid = 0;

	gfx_v10_0_gfx_mqd_init_mcbp(ring);

	/* set up gfx hqd base. this is similar as CP_RB_BASE */
	hqd_gpu_addr = ring->gpu_addr >> 8;
	mqd->cp_gfx_hqd_base = hqd_gpu_addr;
	mqd->cp_gfx_hqd_base_hi = upper_32_bits(hqd_gpu_addr);

	/* set up hqd_rptr_addr/_hi, similar as CP_RB_RPTR */
	wb_gpu_addr = adev->wb.gpu_addr + (ring->rptr_offs * 4);
	mqd->cp_gfx_hqd_rptr_addr = wb_gpu_addr & 0xfffffffc;
	mqd->cp_gfx_hqd_rptr_addr_hi =
		upper_32_bits(wb_gpu_addr) & 0xffff;

	/* set up rb_wptr_poll addr */
	wb_gpu_addr = adev->wb.gpu_addr + (ring->wptr_offs * 4);
	mqd->cp_rb_wptr_poll_addr_lo = wb_gpu_addr & 0xfffffffc;
	mqd->cp_rb_wptr_poll_addr_hi = upper_32_bits(wb_gpu_addr) & 0xffff;

	/* set up the gfx_hqd_control, similar as CP_RB0_CNTL */
	rb_bufsz = order_base_2(ring->ring_size / 4) - 1;
	tmp = RREG32_SOC15(GC, 0, mmCP_GFX_HQD_CNTL);
	tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BUFSZ, rb_bufsz);
	tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BLKSZ, rb_bufsz - 2);
#ifdef __BIG_ENDIAN
	tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, BUF_SWAP, 1);
#endif
	mqd->cp_gfx_hqd_cntl = tmp;

	/* set up cp_doorbell_control */
	tmp = RREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_CONTROL);
	if (ring->use_doorbell) {
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL,
				    DOORBELL_OFFSET, ring->doorbell_index);
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL,
				    DOORBELL_EN, 1);
	} else
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL,
				    DOORBELL_EN, 0);
	mqd->cp_rb_doorbell_control = tmp;

	/* reset read and write pointers, similar to CP_RB0_WPTR/_RPTR */
	ring->wptr = 0;
	mqd->cp_gfx_hqd_rptr = RREG32_SOC15(GC, 0, mmCP_GFX_HQD_RPTR);

	/* active the queue */
	mqd->cp_gfx_hqd_active = 1;

	return 0;
}

static int gfx_v10_0_gfx_queue_init_register(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_gfx_mqd *mqd = ring->mqd_ptr;

	/* set mmCP_GFX_HQD_WPTR/_HI to 0 */
	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_WPTR, mqd->cp_gfx_hqd_wptr);
	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_WPTR_HI, mqd->cp_gfx_hqd_wptr_hi);

	/* set GFX_MQD_BASE */
	WREG32_SOC15(GC, 0, mmCP_GFX_MQD_BASE_ADDR, mqd->cp_mqd_base_addr);
	WREG32_SOC15(GC, 0, mmCP_GFX_MQD_BASE_ADDR_HI, mqd->cp_mqd_base_addr_hi);

	/* set GFX_MQD_CONTROL */
	WREG32_SOC15(GC, 0, mmCP_GFX_MQD_CONTROL, mqd->cp_gfx_mqd_control);

	/* set GFX_HQD_VMID to 0 */
	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_VMID, mqd->cp_gfx_hqd_vmid);

	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_QUEUE_PRIORITY,
			mqd->cp_gfx_hqd_queue_priority);
	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_QUANTUM, mqd->cp_gfx_hqd_quantum);

	/* set GFX_HQD_BASE, similar as CP_RB_BASE */
	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_BASE, mqd->cp_gfx_hqd_base);
	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_BASE_HI, mqd->cp_gfx_hqd_base_hi);

	/* set GFX_HQD_RPTR_ADDR, similar as CP_RB_RPTR */
	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_RPTR_ADDR, mqd->cp_gfx_hqd_rptr_addr);
	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_RPTR_ADDR_HI, mqd->cp_gfx_hqd_rptr_addr_hi);

	/* set GFX_HQD_CNTL, similar as CP_RB_CNTL */
	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_CNTL, mqd->cp_gfx_hqd_cntl);

	/* set RB_WPTR_POLL_ADDR */
	WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_ADDR_LO, mqd->cp_rb_wptr_poll_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_ADDR_HI, mqd->cp_rb_wptr_poll_addr_hi);

	/* set RB_DOORBELL_CONTROL */
	WREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_CONTROL, mqd->cp_rb_doorbell_control);

	/* active the queue */
	WREG32_SOC15(GC, 0, mmCP_GFX_HQD_ACTIVE, mqd->cp_gfx_hqd_active);

	return 0;
}

static int gfx_v10_0_gfx_init_queue(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_gfx_mqd *mqd = ring->mqd_ptr;
	int mqd_idx = ring - &adev->gfx.gfx_ring[0];

	if (!amdgpu_in_reset(adev) && !adev->in_suspend) {
		memset((void *)mqd, 0, sizeof(*mqd));
		mutex_lock(&adev->srbm_mutex);
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
		gfx_v10_0_gfx_mqd_init(ring);
#ifdef BRING_UP_DEBUG
		gfx_v10_0_gfx_queue_init_register(ring);
#endif
		nv_grbm_select(adev, 0, 0, 0, 0);
		mutex_unlock(&adev->srbm_mutex);
		if (adev->gfx.me.mqd_backup[mqd_idx])
			memcpy(adev->gfx.me.mqd_backup[mqd_idx], mqd, sizeof(*mqd));
	} else if (amdgpu_in_reset(adev)) {
		/* reset mqd with the backup copy */
		if (adev->gfx.me.mqd_backup[mqd_idx])
			memcpy(mqd, adev->gfx.me.mqd_backup[mqd_idx], sizeof(*mqd));
		/* reset the ring */
		ring->wptr = 0;
		adev->wb.wb[ring->wptr_offs] = 0;
		amdgpu_ring_clear_ring(ring);
#ifdef BRING_UP_DEBUG
		mutex_lock(&adev->srbm_mutex);
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
		gfx_v10_0_gfx_queue_init_register(ring);
		nv_grbm_select(adev, 0, 0, 0, 0);
		mutex_unlock(&adev->srbm_mutex);
#endif
	} else {
		amdgpu_ring_clear_ring(ring);
	}

	return 0;
}

#ifndef BRING_UP_DEBUG
static int gfx_v10_0_kiq_enable_kgq(struct amdgpu_device *adev)
{
	struct amdgpu_kiq *kiq = &adev->gfx.kiq;
	struct amdgpu_ring *kiq_ring = &adev->gfx.kiq.ring;
	int r, i;

	if (!kiq->pmf || !kiq->pmf->kiq_map_queues)
		return -EINVAL;

	r = amdgpu_ring_alloc(kiq_ring, kiq->pmf->map_queues_size *
					adev->gfx.num_gfx_rings);
	if (r) {
		DRM_ERROR("Failed to lock KIQ (%d).\n", r);
		return r;
	}

	for (i = 0; i < adev->gfx.num_gfx_rings; i++)
		kiq->pmf->kiq_map_queues(kiq_ring, &adev->gfx.gfx_ring[i]);

	return amdgpu_ring_test_helper(kiq_ring);
}
#endif

static int gfx_v10_0_cp_async_gfx_start(struct amdgpu_device *adev)
{
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;
	struct amdgpu_ring *ring;
	int ctx_reg_offset;
	int r, i, k;
	uint32_t process_quantum;

	/* init the CP */
	process_quantum = RREG32_SOC15(GC, 0, mmCP_PROCESS_QUANTUM);
	process_quantum = REG_SET_FIELD(process_quantum, CP_PROCESS_QUANTUM,
					QUANTUM_DURATION, 0x80);
	process_quantum = REG_SET_FIELD(process_quantum, CP_PROCESS_QUANTUM,
					QUANTUM_EN, 0x1);
	process_quantum = REG_SET_FIELD(process_quantum, CP_PROCESS_QUANTUM,
					QUANTUM_SCALE, 0x2);
	WREG32_SOC15(GC, 0, mmCP_PROCESS_QUANTUM, process_quantum);

	WREG32_SOC15(GC, 0, mmCP_MAX_CONTEXT,
		     adev->gfx.config.max_hw_contexts - 1);
	WREG32_SOC15(GC, 0, mmCP_DEVICE_ID, 1);

	gfx_v10_0_cp_gfx_enable(adev, true);

	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		ring = &adev->gfx.gfx_ring[i];
		r = amdgpu_ring_alloc(ring, gfx_v10_0_get_csb_size(adev) + 4);
		if (r) {
			DRM_ERROR("amdgpu: cp failed to alock ring (%d).\n", r);
			return r;
		}

		if (adev->family == AMDGPU_FAMILY_MGFX)
			goto skip_clear_state;

		amdgpu_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
		amdgpu_ring_write(ring, PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);

		amdgpu_ring_write(ring, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
		amdgpu_ring_write(ring, 0x80000000);
		amdgpu_ring_write(ring, 0x80000000);

		for (sect = gfx10_cs_data; sect->section != NULL; ++sect) {
			for (ext = sect->section; ext->extent != NULL; ++ext) {
				if (sect->id == SECT_CONTEXT) {
					amdgpu_ring_write(ring,
						PACKET3(PACKET3_SET_CONTEXT_REG,
						ext->reg_count));
					amdgpu_ring_write(ring, ext->reg_index -
						PACKET3_SET_CONTEXT_REG_START);
					for (k = 0; k < ext->reg_count; k++)
						amdgpu_ring_write(ring,
								ext->extent[k]);
				}
			}
		}

		ctx_reg_offset =
			SOC15_REG_OFFSET(GC, 0, mmPA_SC_TILE_STEERING_OVERRIDE)
			- PACKET3_SET_CONTEXT_REG_START;
		amdgpu_ring_write(ring, PACKET3(PACKET3_SET_CONTEXT_REG, 1));
		amdgpu_ring_write(ring, ctx_reg_offset);
		amdgpu_ring_write(ring,
				adev->gfx.config.pa_sc_tile_steering_override);

		amdgpu_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
		amdgpu_ring_write(ring, PACKET3_PREAMBLE_END_CLEAR_STATE);

		amdgpu_ring_write(ring, PACKET3(PACKET3_CLEAR_STATE, 0));
		amdgpu_ring_write(ring, 0);

skip_clear_state:
		amdgpu_ring_write(ring, PACKET3(PACKET3_SET_BASE, 2));
		amdgpu_ring_write(ring, PACKET3_BASE_INDEX(CE_PARTITION_BASE));
		amdgpu_ring_write(ring, 0x8000);
		amdgpu_ring_write(ring, 0x8000);

		amdgpu_ring_commit(ring);
		DRM_DEBUG("%s: me%d, pipe%d, queue%d\n", __func__,
				ring->me, ring->pipe, ring->queue);
	}

	return 0;
}

static int gfx_v10_0_async_kgq_resume_pio(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring;
	struct v10_gfx_mqd *mqd;
	int r, i;

	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		ring = &adev->gfx.gfx_ring[i];
		mqd = ring->mqd_ptr;
		if (!mqd)
			return -ENOMEM;

		if (!amdgpu_in_reset(adev) && !adev->in_suspend) {
			memset((void *)mqd, 0, sizeof(*mqd));
			mutex_lock(&adev->srbm_mutex);
			nv_grbm_select(adev, ring->me, ring->pipe, ring->queue,
					0);
			gfx_v10_0_gfx_mqd_init(ring);
			gfx_v10_0_gfx_queue_init_register(ring);
			nv_grbm_select(adev, 0, 0, 0, 0);
			mutex_unlock(&adev->srbm_mutex);
			if (adev->gfx.me.mqd_backup[i])
				memcpy(adev->gfx.me.mqd_backup[i],
				       mqd, sizeof(*mqd));
			DRM_DEBUG("%s: me%d, pipe%d, queue%d\n", __func__,
					ring->me, ring->pipe, ring->queue);
		} else if (amdgpu_in_reset(adev)) {
			/* reset mqd with the backup copy */
			if (adev->gfx.me.mqd_backup[i])
				memcpy(mqd, adev->gfx.me.mqd_backup[i],
				       sizeof(*mqd));
			/* reset the ring */
			ring->wptr = 0;
			amdgpu_ring_clear_ring(ring);
			mutex_lock(&adev->srbm_mutex);
			nv_grbm_select(adev, ring->me, ring->pipe, ring->queue,
					0);
			gfx_v10_0_gfx_queue_init_register(ring);
			nv_grbm_select(adev, 0, 0, 0, 0);
			mutex_unlock(&adev->srbm_mutex);
		} else {
			amdgpu_ring_clear_ring(ring);
			memset((void *)mqd, 0, sizeof(*mqd));
			mutex_lock(&adev->srbm_mutex);
			nv_grbm_select(adev, ring->me, ring->pipe, ring->queue,
					0);
			gfx_v10_0_gfx_mqd_init(ring);
			gfx_v10_0_gfx_queue_init_register(ring);
			nv_grbm_select(adev, 0, 0, 0, 0);
			mutex_unlock(&adev->srbm_mutex);
			if (adev->gfx.me.mqd_backup[i])
				memcpy(adev->gfx.me.mqd_backup[i],
						mqd, sizeof(*mqd));
			DRM_DEBUG("%s: me%d, pipe%d, queue%d\n", __func__,
					ring->me, ring->pipe, ring->queue);
		}
	}

	r = gfx_v10_0_cp_async_gfx_start(adev);
	if (r) {
		DRM_ERROR("gfx_v10_0_cp_async_gfx_start failed!\n");
		return r;
	}

	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		ring = &adev->gfx.gfx_ring[i];
		ring->sched.ready = true;
	}

	return 0;

}

static int gfx_v10_0_cp_async_gfx_ring_resume(struct amdgpu_device *adev)
{
	int r, i;
	struct amdgpu_ring *ring;

	if (adev->asic_type == CHIP_VANGOGH_LITE)
		return gfx_v10_0_async_kgq_resume_pio(adev);

	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		ring = &adev->gfx.gfx_ring[i];

		/* On VANGOGH_LITE, MQD's cpu addr will be set to PIO when
		 * mapping amdgpu_bo_kmap should not be called on it
		 * amdgpu_gfx_mqd_sw_fini will release it correctly
		 */
		if (adev->asic_type == CHIP_VANGOGH_LITE)
			r = gfx_v10_0_gfx_init_queue(ring);
		else {
			r = amdgpu_bo_reserve(ring->mqd_obj, false);
			if (unlikely(r != 0))
				goto done;

			r = amdgpu_bo_kmap(ring->mqd_obj,
					(void **)&ring->mqd_ptr);
			if (!r) {
				r = gfx_v10_0_gfx_init_queue(ring);
				amdgpu_bo_kunmap(ring->mqd_obj);
				ring->mqd_ptr = NULL;
			}
			amdgpu_bo_unreserve(ring->mqd_obj);
		}

		if (r)
			goto done;
	}
#ifndef BRING_UP_DEBUG
	r = gfx_v10_0_kiq_enable_kgq(adev);
	if (r)
		goto done;
#endif
	r = gfx_v10_0_cp_gfx_start(adev);
	if (r)
		goto done;

	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		ring = &adev->gfx.gfx_ring[i];
		ring->sched.ready = true;
	}
done:
	return r;
}

static void gfx_v10_0_compute_mqd_set_priority(struct amdgpu_ring *ring, struct v10_compute_mqd *mqd)
{
	struct amdgpu_device *adev = ring->adev;

	if (ring->funcs->type == AMDGPU_RING_TYPE_COMPUTE) {
		if (amdgpu_gfx_is_high_priority_compute_queue(adev, ring->pipe,
							      ring->queue)) {
			mqd->cp_hqd_pipe_priority = AMDGPU_GFX_PIPE_PRIO_HIGH;
			mqd->cp_hqd_queue_priority =
				AMDGPU_GFX_QUEUE_PRIORITY_MAXIMUM;
		}
	}
}

static int gfx_v10_0_compute_mqd_init(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_compute_mqd *mqd = ring->mqd_ptr;
	uint64_t hqd_gpu_addr, wb_gpu_addr, eop_base_addr;
	u32 tmp, quantum;

	mqd->header = 0xC0310800;
	mqd->compute_pipelinestat_enable = 0x00000001;
	mqd->compute_static_thread_mgmt_se0 = 0xffffffff;
	mqd->compute_static_thread_mgmt_se1 = 0xffffffff;
	mqd->compute_static_thread_mgmt_se2 = 0xffffffff;
	mqd->compute_static_thread_mgmt_se3 = 0xffffffff;
	mqd->compute_misc_reserved = 0x00000003;
	mqd->compute_req_ctrl = 0x00800000;

	eop_base_addr = ring->eop_gpu_addr >> 8;
	mqd->cp_hqd_eop_base_addr_lo = eop_base_addr;
	mqd->cp_hqd_eop_base_addr_hi = upper_32_bits(eop_base_addr);

	/* set the EOP size, register value is 2^(EOP_SIZE+1) dwords */
	tmp = RREG32_SOC15(GC, 0, mmCP_HQD_EOP_CONTROL);
	tmp = REG_SET_FIELD(tmp, CP_HQD_EOP_CONTROL, EOP_SIZE,
			(order_base_2(GFX10_MEC_HPD_SIZE / 4) - 1));

	mqd->cp_hqd_eop_control = tmp;

	/* enable doorbell? */
	tmp = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_DOORBELL_CONTROL);

	if (ring->use_doorbell) {
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL,
				    DOORBELL_OFFSET, ring->doorbell_index);
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL,
				    DOORBELL_EN, 1);
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL,
				    DOORBELL_SOURCE, 0);
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL,
				    DOORBELL_HIT, 0);
	} else {
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL,
				    DOORBELL_EN, 0);
	}

	mqd->cp_hqd_pq_doorbell_control = tmp;

	/* disable the queue if it's active */
	mqd->cp_hqd_dequeue_request = 0;
	mqd->cp_hqd_pq_rptr = 0;
	mqd->cp_hqd_pq_wptr_lo = 0;
	mqd->cp_hqd_pq_wptr_hi = 0;
	mqd->cp_hqd_hq_status0 = 0x001002c0;
	mqd->cp_hqd_hq_control0 = 0;
	mqd->cp_hqd_hq_status1 = 0;
	mqd->cp_hqd_hq_control1 = 0;

	/* set the pointer to the MQD */
	mqd->cp_mqd_base_addr_lo = ring->mqd_gpu_addr & 0xfffffffc;
	mqd->cp_mqd_base_addr_hi = upper_32_bits(ring->mqd_gpu_addr);

	/* set MQD vmid to 0 */
	tmp = RREG32_SOC15(GC, 0, mmCP_MQD_CONTROL);
	if (ring->cwsr || ring->tmz)
		tmp = REG_SET_FIELD(tmp, CP_MQD_CONTROL, VMID, ring->priv_vmid);
	else
		tmp = REG_SET_FIELD(tmp, CP_MQD_CONTROL, VMID, 0);
	mqd->cp_mqd_control = tmp;

	/* set the pointer to the HQD, this is similar CP_RB0_BASE/_HI */
	hqd_gpu_addr = ring->gpu_addr >> 8;
	mqd->cp_hqd_pq_base_lo = hqd_gpu_addr;
	mqd->cp_hqd_pq_base_hi = upper_32_bits(hqd_gpu_addr);

	/* set up the HQD, this is similar to CP_RB0_CNTL */
	tmp = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_CONTROL);
	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, QUEUE_SIZE,
			    (order_base_2(ring->ring_size / 4) - 1));
	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE,
			    ((order_base_2(AMDGPU_GPU_PAGE_SIZE / 4) - 1) << 8));
#ifdef __BIG_ENDIAN
	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, ENDIAN_SWAP, 1);
#endif
	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 0);

	/* Enable ACE Tunnel only on first pipe/queue to make sure tunneling
	 * is not enabled more than one queue simultaneously as per HW spec
	 */
	if (ring->me == 1 && ring->pipe == 0 && ring->queue == 0) {
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 1);
		DRM_DEBUG("ACE tunnel enabled on me:%d pipe:%d queue:%d\n",
			 ring->me, ring->pipe, ring->queue);
	} else {
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 0);
	}

	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
	if (ring->tmz || ring->cwsr)
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, KMD_QUEUE, 0);
	else
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
	mqd->cp_hqd_pq_control = tmp;

	/* set the wb address whether it's enabled or not */
	if (ring->cwsr || ring->tmz)
		wb_gpu_addr = ring->rptr_gpu_addr;
	else
		wb_gpu_addr = adev->wb.gpu_addr + (ring->rptr_offs * 4);
	mqd->cp_hqd_pq_rptr_report_addr_lo = wb_gpu_addr & 0xfffffffc;
	mqd->cp_hqd_pq_rptr_report_addr_hi =
		upper_32_bits(wb_gpu_addr) & 0xffff;

	/* only used if CP_PQ_WPTR_POLL_CNTL.CP_PQ_WPTR_POLL_CNTL__EN_MASK=1 */
	if (ring->cwsr || ring->tmz)
		wb_gpu_addr = ring->wptr_gpu_addr;
	else
		wb_gpu_addr = adev->wb.gpu_addr + (ring->wptr_offs * 4);
	mqd->cp_hqd_pq_wptr_poll_addr_lo = wb_gpu_addr & 0xfffffffc;
	mqd->cp_hqd_pq_wptr_poll_addr_hi = upper_32_bits(wb_gpu_addr) & 0xffff;

	tmp = 0;
	/* enable the doorbell if requested */
	if (ring->use_doorbell) {
		tmp = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_DOORBELL_CONTROL);
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL,
				DOORBELL_OFFSET, ring->doorbell_index);

		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL,
				    DOORBELL_EN, 1);
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL,
				    DOORBELL_SOURCE, 0);
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL,
				    DOORBELL_HIT, 0);
	}

	mqd->cp_hqd_pq_doorbell_control = tmp;

	/* set the vmid for the queue */
	if (ring->cwsr || ring->tmz) {
		tmp = REG_SET_FIELD(0, CP_HQD_VMID,
				    VMID,
				    ring->priv_vmid);

		tmp = REG_SET_FIELD(tmp, CP_HQD_VMID,
				    IB_VMID,
				    ring->priv_vmid);
		mqd->cp_hqd_vmid = tmp;
	} else {
		mqd->cp_hqd_vmid = 0;
	}

	tmp = RREG32_SOC15(GC, 0, mmCP_HQD_PERSISTENT_STATE);
	tmp = REG_SET_FIELD(tmp, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x53);
	tmp = REG_SET_FIELD(tmp, CP_HQD_PERSISTENT_STATE, RELAUNCH_WAVES, 0x0);
	tmp = REG_SET_FIELD(tmp, CP_HQD_PERSISTENT_STATE, QSWITCH_MODE, 0x0);
	mqd->cp_hqd_persistent_state = tmp;

	/* set MIN_IB_AVAIL_SIZE */
	tmp = RREG32_SOC15(GC, 0, mmCP_HQD_IB_CONTROL);
	tmp = REG_SET_FIELD(tmp, CP_HQD_IB_CONTROL, MIN_IB_AVAIL_SIZE, 3);
	mqd->cp_hqd_ib_control = tmp;

	if (ring->cwsr) {
		mqd->cp_hqd_pipe_priority = 0;
		mqd->cp_hqd_queue_priority = 0;
		switch (ring->sws_ctx.priority) {
		case SWS_SCHED_PRIORITY_HIGH:
			quantum = 20;
			break;

		case SWS_SCHED_PRIORITY_NORMAL:
			quantum = 15;
			break;

		case SWS_SCHED_PRIORITY_LOW:
			quantum = 10;
			break;

		default:
			DRM_WARN("abnormal priority(%u) setting!\n",
				 ring->sws_ctx.priority);
			quantum = 10;
			break;
		}

		tmp = RREG32_SOC15(GC, 0, mmCP_HQD_QUANTUM);
		tmp = REG_SET_FIELD(tmp, CP_HQD_QUANTUM, QUANTUM_EN, 1);
		tmp = REG_SET_FIELD(tmp, CP_HQD_QUANTUM, QUANTUM_SCALE, 1);
		tmp = REG_SET_FIELD(tmp, CP_HQD_QUANTUM, QUANTUM_DURATION,
				    quantum);
		mqd->cp_hqd_quantum = tmp;
	} else {
		/* set static priority for a compute queue/ring */
		gfx_v10_0_compute_mqd_set_priority(ring, mqd);
	}

	if (ring->cwsr) {
		mqd->cp_hqd_persistent_state |=
			(1 << CP_HQD_PERSISTENT_STATE__QSWITCH_MODE__SHIFT);

		mqd->cp_hqd_ctx_save_base_addr_lo =
			lower_32_bits(ring->cwsr_sr_gpu_addr);
		mqd->cp_hqd_ctx_save_base_addr_hi =
			upper_32_bits(ring->cwsr_sr_gpu_addr);
		mqd->cp_hqd_ctx_save_size = ring->cwsr_sr_size;
		mqd->cp_hqd_cntl_stack_size = ring->cwsr_sr_ctl_size;
		mqd->cp_hqd_cntl_stack_offset = ring->cwsr_sr_ctl_size;
		mqd->cp_hqd_wg_state_offset = ring->cwsr_sr_ctl_size;
	}

	/* map_queues packet doesn't need activate the queue,
	 * so only kiq need set this field.
	 */
	if (ring->funcs->type == AMDGPU_RING_TYPE_KIQ)
		mqd->cp_hqd_active = 1;

	return 0;
}

static int gfx_v10_0_compute_mqd_update(struct amdgpu_ring *ring)
{
	u32 tmp;
	struct amdgpu_device *adev;
	struct v10_compute_mqd *mqd;

	mqd = ring->mqd_ptr;
	adev = ring->adev;

	/* update vmid of hqd */
	if (ring->tmz) {
		tmp = mqd->cp_mqd_control;
		tmp = REG_SET_FIELD(tmp, CP_MQD_CONTROL, VMID, ring->priv_vmid);
		mqd->cp_mqd_control = tmp;

		tmp = mqd->cp_hqd_vmid;
		tmp = REG_SET_FIELD(tmp, CP_HQD_VMID,
				    VMID,
				    ring->priv_vmid);
		tmp = REG_SET_FIELD(tmp, CP_HQD_VMID,
				    IB_VMID,
				    ring->priv_vmid);
		mqd->cp_hqd_vmid = tmp;
	}

	return 0;
}

static int gfx_v10_0_compute_cwsr_mqd_init(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev;
	struct v10_compute_mqd *mqd;

	mqd = ring->mqd_ptr;
	adev = ring->adev;

	memset((void *)mqd, 0, sizeof(*mqd));

	mutex_lock(&adev->srbm_mutex);
	if (ring->cwsr || ring->tmz)
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue,
			       ring->priv_vmid);
	else
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);

	gfx_v10_0_compute_mqd_init(ring);
	nv_grbm_select(adev, 0, 0, 0, 0);
	mutex_unlock(&adev->srbm_mutex);

	return 0;
}

static int gfx_v10_0_kiq_init_register(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_compute_mqd *mqd = ring->mqd_ptr;
	u32 tba_lo_addr, tba_hi_addr;

	/* inactivate the queue */
	if (amdgpu_sriov_vf(adev))
		WREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE, 0);

	/* disable the queue if it's active */
	if (RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE) & 1) {
		int i;

		DRM_WARN("reinit an active ring %s on %u.%u.%u!!!\n",
			 ring->name, ring->me, ring->pipe, ring->queue);
		WREG32_SOC15(GC, 0, mmCP_HQD_DEQUEUE_REQUEST, 1);
		for (i = 0; i < adev->usec_timeout; i++) {
			if (!(RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE) & 1))
				break;
			udelay(1);
		}
	}

	/* set the pointer to the MQD */
	WREG32_SOC15(GC, 0, mmCP_MQD_BASE_ADDR, mqd->cp_mqd_base_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_MQD_BASE_ADDR_HI, mqd->cp_mqd_base_addr_hi);

	/* set the vmid for the queue */
	WREG32_SOC15(GC, 0, mmCP_HQD_VMID, mqd->cp_hqd_vmid);

	/* set continuous QD registers from HQD_PERSISTENT_STATE to wrptr_poll_addr_hi */
	WREG32_SOC15(GC, 0, mmCP_HQD_PERSISTENT_STATE, mqd->cp_hqd_persistent_state);

	/* priority */
	WREG32_SOC15(GC, 0, mmCP_HQD_PIPE_PRIORITY, mqd->cp_hqd_pipe_priority);
	WREG32_SOC15(GC, 0, mmCP_HQD_QUEUE_PRIORITY, mqd->cp_hqd_queue_priority);
	WREG32_SOC15(GC, 0, mmCP_HQD_QUANTUM, mqd->cp_hqd_quantum);

	/* set the pointer to the HQD, this is similar CP_RB0_BASE/_HI */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_BASE, mqd->cp_hqd_pq_base_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_BASE_HI, mqd->cp_hqd_pq_base_hi);

	/* set the wb address whether it's enabled or not */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR, mqd->cp_hqd_pq_rptr);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR_REPORT_ADDR, mqd->cp_hqd_pq_rptr_report_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI, mqd->cp_hqd_pq_rptr_report_addr_hi);

	/* only useful if CP_PQ_WPTR_POLL_CNTL.CP_PQ_WPTR_POLL_CNTL__EN_MASK=1 */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_POLL_ADDR, mqd->cp_hqd_pq_wptr_poll_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_POLL_ADDR_HI, mqd->cp_hqd_pq_wptr_poll_addr_hi);

	/* Set doorbell offset, but ensure doorbell is not enabled yet */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_DOORBELL_CONTROL,
		     mqd->cp_hqd_pq_doorbell_control & 0x3fffffff);

	/* set continuous QD registers from mmCP_HQD_PQ_CONTROL to mmCP_HQD_HQ_CONTROL1 */

	/* set up the HQD, this is similar to CP_RB0_CNTL */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_CONTROL, mqd->cp_hqd_pq_control);

	/* ib */
	WREG32_SOC15(GC, 0, mmCP_HQD_IB_BASE_ADDR, mqd->cp_hqd_ib_base_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_IB_BASE_ADDR_HI, mqd->cp_hqd_ib_base_addr_hi);
	WREG32_SOC15(GC, 0, mmCP_HQD_IB_RPTR, mqd->cp_hqd_ib_rptr);
	WREG32_SOC15(GC, 0, mmCP_HQD_IB_CONTROL, mqd->cp_hqd_ib_control);

	WREG32_SOC15(GC, 0, mmCP_HQD_IQ_TIMER, mqd->cp_hqd_iq_timer);
	WREG32_SOC15(GC, 0, mmCP_HQD_IQ_RPTR, mqd->cp_hqd_iq_rptr);
	WREG32_SOC15(GC, 0, mmCP_HQD_DEQUEUE_REQUEST, mqd->cp_hqd_dequeue_request);
	WREG32_SOC15(GC, 0, mmCP_HQD_DMA_OFFLOAD, mqd->cp_hqd_dma_offload);
	WREG32_SOC15(GC, 0, mmCP_HQD_MSG_TYPE, mqd->cp_hqd_msg_type);

	WREG32_SOC15(GC, 0, mmCP_HQD_ATOMIC0_PREOP_LO, mqd->cp_hqd_atomic0_preop_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_ATOMIC0_PREOP_HI, mqd->cp_hqd_atomic0_preop_hi);
	WREG32_SOC15(GC, 0, mmCP_HQD_ATOMIC1_PREOP_LO, mqd->cp_hqd_atomic1_preop_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_ATOMIC1_PREOP_HI, mqd->cp_hqd_atomic1_preop_hi);

	/*
	 * mmCP_HQD_HQ_STATUS0 is used by CP FW for HWS and other purpose,
	 * setting this by KMD will cause unexpected issues
	 */
	WREG32_SOC15(GC, 0, mmCP_HQD_HQ_CONTROL0, mqd->cp_hqd_hq_control0);

	/* set MQD vmid */
	WREG32_SOC15(GC, 0, mmCP_MQD_CONTROL, mqd->cp_mqd_control);

	WREG32_SOC15(GC, 0, mmCP_HQD_HQ_STATUS1, mqd->cp_hqd_hq_status1);
	WREG32_SOC15(GC, 0, mmCP_HQD_HQ_CONTROL1, mqd->cp_hqd_hq_control1);

	/* set continuous QD registers from mmCP_HQD_EOP_BASE_ADDR to mmCP_HQD_DEQUEUE_STATUS */

	/* write the EOP addr */
	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_BASE_ADDR, mqd->cp_hqd_eop_base_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_BASE_ADDR_HI, mqd->cp_hqd_eop_base_addr_hi);

	/* set the EOP size, register value is 2^(EOP_SIZE+1) dwords */
	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_CONTROL, mqd->cp_hqd_eop_control);

	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_RPTR, mqd->cp_hqd_eop_rptr);
	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_WPTR, mqd->cp_hqd_eop_wptr);
	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_EVENTS, mqd->cp_hqd_eop_done_events);

	/* SR setting for CWSR */
	WREG32_SOC15(GC, 0, mmCP_HQD_CTX_SAVE_BASE_ADDR_LO, mqd->cp_hqd_ctx_save_base_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_CTX_SAVE_BASE_ADDR_HI, mqd->cp_hqd_ctx_save_base_addr_hi);
	WREG32_SOC15(GC, 0, mmCP_HQD_CTX_SAVE_CONTROL, mqd->cp_hqd_ctx_save_control);
	WREG32_SOC15(GC, 0, mmCP_HQD_CNTL_STACK_OFFSET, mqd->cp_hqd_cntl_stack_offset);
	WREG32_SOC15(GC, 0, mmCP_HQD_CNTL_STACK_SIZE, mqd->cp_hqd_cntl_stack_size);
	WREG32_SOC15(GC, 0, mmCP_HQD_WG_STATE_OFFSET, mqd->cp_hqd_wg_state_offset);
	WREG32_SOC15(GC, 0, mmCP_HQD_CTX_SAVE_SIZE, mqd->cp_hqd_ctx_save_size);
	WREG32_SOC15(GC, 0, mmCP_HQD_GDS_RESOURCE_STATE, mqd->cp_hqd_gds_resource_state);
	WREG32_SOC15(GC, 0, mmCP_HQD_ERROR, mqd->cp_hqd_error);
	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_WPTR_MEM, mqd->cp_hqd_eop_wptr_mem);

	WREG32_SOC15(GC, 0, mmCP_HQD_AQL_CONTROL, mqd->cp_hqd_aql_control);

	/*
	 * with dynamic queue map/unmap, mqd->cp_hqd_pq_wptr_lo/hi may not be updated properly
	 * due to pending submissions processed by CP FW, KMD need to get updated value from
	 * the memory location of wptr directly or for FW to update using POLL_CNTL1
	 * Since using POLL_CNTL1 to force update need to sync with FW for the update, it's more
	 * straight forward reading it out through memory location
	 */
	if (ring->cwsr || ring->tmz) {
		mqd->cp_hqd_pq_wptr_hi = upper_32_bits(gfx_v10_0_ring_get_wptr_compute(ring));
		mqd->cp_hqd_pq_wptr_lo = lower_32_bits(gfx_v10_0_ring_get_wptr_compute(ring));
	}
	/*
	 * Store to POLL_CNTL1 to force a read of the write pointer,
	 * MEC engine only because MES does not have the same RTL supported
	 */

	/* reset read and write pointers, similar to CP_RB0_WPTR/_RPTR */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_LO, mqd->cp_hqd_pq_wptr_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_HI, mqd->cp_hqd_pq_wptr_hi);

	/*
	 * This is used for CP suspend/resume, hardware default is 0, setting them here
	 * make no difference, need to further verify when CP suspend/resume is used
	 */

	WREG32_SOC15(GC, 0, mmCP_HQD_DDID_RPTR, mqd->reserved_187);
	WREG32_SOC15(GC, 0, mmCP_HQD_DDID_WPTR, mqd->reserved_188);
	WREG32_SOC15(GC, 0, mmCP_HQD_DDID_INFLIGHT_COUNT, mqd->reserved_189);
	WREG32_SOC15(GC, 0, mmCP_HQD_DDID_DELTA_RPT_COUNT, mqd->reserved_190);
	WREG32_SOC15(GC, 0, mmCP_HQD_DEQUEUE_STATUS, mqd->reserved_191);
	/* End of Program HQD from MQD */

	/* Start the EOP fetcher */
	mqd->cp_hqd_eop_rptr |= REG_SET_FIELD(mqd->cp_hqd_eop_rptr,
					      CP_HQD_EOP_RPTR, INIT_FETCHER, 1);
	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_RPTR, mqd->cp_hqd_eop_rptr);

	/* disable wptr polling */
	WREG32_FIELD15(GC, 0, CP_PQ_WPTR_POLL_CNTL, EN, 0);

	/* config the doorbell if requested */
	if (ring->use_doorbell) {
		WREG32_SOC15(GC, 0, mmCP_MEC_DOORBELL_RANGE_LOWER,
			     (adev->doorbell_index.kiq * 2) << 2);
		if (cwsr_enable || amdgpu_tmz)
			WREG32_SOC15(GC, 0, mmCP_MEC_DOORBELL_RANGE_UPPER,
				     (adev->doorbell_index.last_resv * 2) << 2);
		else
			WREG32_SOC15(GC, 0, mmCP_MEC_DOORBELL_RANGE_UPPER,
				     (adev->doorbell_index.userqueue_end * 2) << 2);
		WREG32_FIELD15(GC, 0, CP_PQ_WPTR_POLL_CNTL, EN, 0);
	} else {
		WREG32_FIELD15(GC, 0, CP_PQ_WPTR_POLL_CNTL, EN, 1);
	}

	/* set doorbell */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_DOORBELL_CONTROL, mqd->cp_hqd_pq_doorbell_control);

	tba_lo_addr = lower_32_bits(ring->cwsr_tba_gpu_addr >> 8);
	tba_hi_addr = upper_32_bits(ring->cwsr_tba_gpu_addr >> 8) & SQ_SHADER_TBA_HI__ADDR_HI_MASK;
	tba_hi_addr |= 1 << SQ_SHADER_TBA_HI__TRAP_EN__SHIFT;
	if (adev->asic_type == CHIP_VANGOGH_LITE) {
		WREG32_SOC15(GC, 0, mmM0_SQ_SHADER_TBA_LO, tba_lo_addr);
		WREG32_SOC15(GC, 0, mmM0_SQ_SHADER_TBA_HI, tba_hi_addr);
	} else {
		WREG32_SOC15(GC, 0, mmSQ_SHADER_TBA_LO, tba_lo_addr);
		WREG32_SOC15(GC, 0, mmSQ_SHADER_TBA_HI, tba_hi_addr);
	}

	/* activate the queue */
	WREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE, mqd->cp_hqd_active);

	if (ring->use_doorbell)
		WREG32_FIELD15(GC, 0, CP_PQ_STATUS, DOORBELL_ENABLE, 1);

	if (atomic_read(&ring->fence_drv.last_seq) !=
	    ring->fence_drv.sync_seq)
		WDOORBELL64(ring->doorbell_index, ring->wptr);

	return 0;
}

static int gfx_v10_0_kiq_init_queue(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_compute_mqd *mqd = ring->mqd_ptr;
	int mqd_idx = AMDGPU_MAX_COMPUTE_RINGS;

	gfx_v10_0_kiq_setting(ring);

	if (amdgpu_in_reset(adev)) { /* for GPU_RESET case */
		/* reset MQD to a clean status */
		if (adev->gfx.mec.mqd_backup[mqd_idx])
			memcpy(mqd, adev->gfx.mec.mqd_backup[mqd_idx], sizeof(*mqd));

		/* reset ring buffer */
		ring->wptr = 0;
		amdgpu_ring_clear_ring(ring);

		mutex_lock(&adev->srbm_mutex);
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
		gfx_v10_0_kiq_init_register(ring);
		nv_grbm_select(adev, 0, 0, 0, 0);
		mutex_unlock(&adev->srbm_mutex);
	} else {
		memset((void *)mqd, 0, sizeof(*mqd));
		mutex_lock(&adev->srbm_mutex);
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
		gfx_v10_0_compute_mqd_init(ring);
		gfx_v10_0_kiq_init_register(ring);
		nv_grbm_select(adev, 0, 0, 0, 0);
		mutex_unlock(&adev->srbm_mutex);

		if (adev->gfx.mec.mqd_backup[mqd_idx])
			memcpy(adev->gfx.mec.mqd_backup[mqd_idx], mqd, sizeof(*mqd));
	}

	return 0;
}

static int gfx_v10_0_kcq_init_queue(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_compute_mqd *mqd = ring->mqd_ptr;
	int mqd_idx = ring - &adev->gfx.compute_ring[0];

	if (!amdgpu_in_reset(adev) && !adev->in_suspend) {
		memset((void *)mqd, 0, sizeof(*mqd));
		mutex_lock(&adev->srbm_mutex);
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
		gfx_v10_0_compute_mqd_init(ring);
		nv_grbm_select(adev, 0, 0, 0, 0);
		mutex_unlock(&adev->srbm_mutex);

		if (adev->gfx.mec.mqd_backup[mqd_idx])
			memcpy(adev->gfx.mec.mqd_backup[mqd_idx], mqd, sizeof(*mqd));
	} else if (amdgpu_in_reset(adev)) { /* for GPU_RESET case */
		/* reset MQD to a clean status */
		if (adev->gfx.mec.mqd_backup[mqd_idx])
			memcpy(mqd, adev->gfx.mec.mqd_backup[mqd_idx], sizeof(*mqd));

		memset((void *)mqd, 0, sizeof(*mqd));
		mutex_lock(&adev->srbm_mutex);
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
		gfx_v10_0_compute_mqd_init(ring);
		nv_grbm_select(adev, 0, 0, 0, 0);
		mutex_unlock(&adev->srbm_mutex);

		/* reset ring buffer */
		ring->wptr = 0;
		atomic64_set((atomic64_t *)&adev->wb.wb[ring->wptr_offs], 0);
		amdgpu_ring_clear_ring(ring);
	} else {
		memset((void *)mqd, 0, sizeof(*mqd));
		mutex_lock(&adev->srbm_mutex);
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
		gfx_v10_0_compute_mqd_init(ring);
		nv_grbm_select(adev, 0, 0, 0, 0);
		mutex_unlock(&adev->srbm_mutex);

		if (adev->gfx.mec.mqd_backup[mqd_idx])
			memcpy(adev->gfx.mec.mqd_backup[mqd_idx], mqd, sizeof(*mqd));
		amdgpu_ring_clear_ring(ring);
	}

	return 0;
}

static int gfx_v10_0_kiq_resume(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring;
	int r;

	ring = &adev->gfx.kiq.ring;

	r = amdgpu_bo_reserve(ring->mqd_obj, false);
	if (unlikely(r != 0))
		return r;

	r = amdgpu_bo_kmap(ring->mqd_obj, (void **)&ring->mqd_ptr);
	if (unlikely(r != 0))
		return r;

	gfx_v10_0_kiq_init_queue(ring);
	amdgpu_bo_kunmap(ring->mqd_obj);
	ring->mqd_ptr = NULL;
	amdgpu_bo_unreserve(ring->mqd_obj);
	ring->sched.ready = true;
	return 0;
}

static int gfx_v10_0_kcq_resume(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring = NULL;
	int r = 0, i;

	gfx_v10_0_cp_compute_enable(adev, true);

	for (i = 0; i < adev->gfx.num_compute_rings; i++) {
		ring = &adev->gfx.compute_ring[i];

		/* On VANGOGH_LITE, MQD's cpu addr will be set to PIO
		 * when mapping amdgpu_bo_kmap should not be called on
		 * it amdgpu_gfx_mqd_sw_fini will release it correctly
		 */
		if (adev->asic_type == CHIP_VANGOGH_LITE)
			r = gfx_v10_0_kcq_init_queue(ring);
		else {

			r = amdgpu_bo_reserve(ring->mqd_obj, false);
			if (unlikely(r != 0))
				goto done;
			r = amdgpu_bo_kmap(ring->mqd_obj,
					(void **)&ring->mqd_ptr);
			if (!r) {
				r = gfx_v10_0_kcq_init_queue(ring);
				amdgpu_bo_kunmap(ring->mqd_obj);
				ring->mqd_ptr = NULL;
			}
			amdgpu_bo_unreserve(ring->mqd_obj);
		}

		if (r)
			goto done;
	}

	r = amdgpu_gfx_enable_kcq(adev);
	if (r)
		goto done;
	for (i = 0; i < adev->gfx.num_compute_rings; i++) {
		ring = &adev->gfx.compute_ring[i];
		ring->sched.ready = true;
	}

done:
	return r;
}

static int gfx_v10_0_cp_resume(struct amdgpu_device *adev)
{
	int r, i;
	struct amdgpu_ring *ring;

	if (!(adev->flags & AMD_IS_APU))
		gfx_v10_0_enable_gui_idle_interrupt(adev, false);

	if (adev->firmware.load_type == AMDGPU_FW_LOAD_DIRECT) {
		/* legacy firmware loading */
		r = gfx_v10_0_cp_gfx_load_microcode(adev);
		if (r)
			return r;

		r = gfx_v10_0_cp_compute_load_microcode(adev);
		if (r)
			return r;
	}

	DRM_DEBUG("Skip KIQ resume for Vangogh!\n");

	r = gfx_v10_0_kcq_resume(adev);
	if (r)
		return r;

	if (!amdgpu_async_gfx_ring) {
		r = gfx_v10_0_cp_gfx_resume(adev);
		if (r)
			return r;
	} else {
		r = gfx_v10_0_cp_async_gfx_ring_resume(adev);
		if (r)
			return r;
	}

	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		ring = &adev->gfx.gfx_ring[i];
		r = amdgpu_ring_test_helper(ring);
		if (r)
			return r;
	}

	for (i = 0; i < adev->gfx.num_compute_rings; i++) {
		ring = &adev->gfx.compute_ring[i];
		r = amdgpu_ring_test_helper(ring);
		if (r)
			return r;
	}

	return 0;
}

static int gfx_v10_0_soft_reset_cp_resume(struct amdgpu_device *adev)
{
	int r, i;
	struct amdgpu_ring *ring;

	if (adev->asic_type != CHIP_VANGOGH_LITE) {
		r = gfx_v10_0_kiq_resume(adev);
		if (r)
			return r;
	} else
		DRM_INFO("Skip KIQ resume!\n");

	r = gfx_v10_0_kcq_resume(adev);
	if (r)
		return r;

	if (!amdgpu_async_gfx_ring) {
		r = gfx_v10_0_cp_gfx_resume(adev);
		if (r)
			return r;
	} else {
		r = gfx_v10_0_cp_async_gfx_ring_resume(adev);
		if (r)
			return r;
	}

	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		ring = &adev->gfx.gfx_ring[i];
		r = amdgpu_ring_test_helper(ring);
		if (r)
			return r;
	}

	for (i = 0; i < adev->gfx.num_compute_rings; i++) {
		ring = &adev->gfx.compute_ring[i];
		r = amdgpu_ring_test_helper(ring);
		if (r)
			return r;
	}

	return 0;
}

static void gfx_v10_0_cp_enable(struct amdgpu_device *adev, bool enable)
{
	gfx_v10_0_cp_gfx_enable(adev, enable);
	gfx_v10_0_cp_compute_enable(adev, enable);
}

static bool gfx_v10_0_check_grbm_cam_remapping(struct amdgpu_device *adev)
{
	uint32_t data, pattern = 0xDEADBEEF;

	/* check if mmVGT_ESGS_RING_SIZE_UMD
	 * has been remapped to mmVGT_ESGS_RING_SIZE */
	data = RREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE);
	WREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE, 0);
	WREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE_UMD, pattern);

	if (RREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE) == pattern) {
		WREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE_UMD, data);
		return true;
	} else {
		WREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE, data);
		return false;
	}
}

static void gfx_v10_0_setup_grbm_cam_remapping(struct amdgpu_device *adev)
{
	uint32_t data;

	/* initialize cam_index to 0
	 * index will auto-inc after each data writting */
	WREG32_SOC15(GC, 0, mmGRBM_CAM_INDEX, 0);

	/* mmVGT_TF_RING_SIZE_UMD -> mmVGT_TF_RING_SIZE */
	data = (SOC15_REG_OFFSET(GC, 0, mmVGT_TF_RING_SIZE_UMD) <<
		GRBM_CAM_DATA__CAM_ADDR__SHIFT) |
	       (SOC15_REG_OFFSET(GC, 0, mmVGT_TF_RING_SIZE) <<
		GRBM_CAM_DATA__CAM_REMAPADDR__SHIFT);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA_UPPER, 0);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA, data);

	/* mmVGT_TF_MEMORY_BASE_UMD -> mmVGT_TF_MEMORY_BASE */
	data = (SOC15_REG_OFFSET(GC, 0, mmVGT_TF_MEMORY_BASE_UMD) <<
		GRBM_CAM_DATA__CAM_ADDR__SHIFT) |
	       (SOC15_REG_OFFSET(GC, 0, mmVGT_TF_MEMORY_BASE) <<
		GRBM_CAM_DATA__CAM_REMAPADDR__SHIFT);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA_UPPER, 0);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA, data);

	/* mmVGT_TF_MEMORY_BASE_HI_UMD -> mmVGT_TF_MEMORY_BASE_HI */
	data = (SOC15_REG_OFFSET(GC, 0, mmVGT_TF_MEMORY_BASE_HI_UMD) <<
		GRBM_CAM_DATA__CAM_ADDR__SHIFT) |
	       (SOC15_REG_OFFSET(GC, 0, mmVGT_TF_MEMORY_BASE_HI) <<
		GRBM_CAM_DATA__CAM_REMAPADDR__SHIFT);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA_UPPER, 0);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA, data);

	/* mmVGT_HS_OFFCHIP_PARAM_UMD -> mmVGT_HS_OFFCHIP_PARAM */
	data = (SOC15_REG_OFFSET(GC, 0, mmVGT_HS_OFFCHIP_PARAM_UMD) <<
		GRBM_CAM_DATA__CAM_ADDR__SHIFT) |
	       (SOC15_REG_OFFSET(GC, 0, mmVGT_HS_OFFCHIP_PARAM) <<
		GRBM_CAM_DATA__CAM_REMAPADDR__SHIFT);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA_UPPER, 0);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA, data);

	/* mmVGT_ESGS_RING_SIZE_UMD -> mmVGT_ESGS_RING_SIZE */
	data = (SOC15_REG_OFFSET(GC, 0, mmVGT_ESGS_RING_SIZE_UMD) <<
		GRBM_CAM_DATA__CAM_ADDR__SHIFT) |
	       (SOC15_REG_OFFSET(GC, 0, mmVGT_ESGS_RING_SIZE) <<
		GRBM_CAM_DATA__CAM_REMAPADDR__SHIFT);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA_UPPER, 0);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA, data);

	/* mmVGT_GSVS_RING_SIZE_UMD -> mmVGT_GSVS_RING_SIZE */
	data = (SOC15_REG_OFFSET(GC, 0, mmVGT_GSVS_RING_SIZE_UMD) <<
		GRBM_CAM_DATA__CAM_ADDR__SHIFT) |
	       (SOC15_REG_OFFSET(GC, 0, mmVGT_GSVS_RING_SIZE) <<
		GRBM_CAM_DATA__CAM_REMAPADDR__SHIFT);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA_UPPER, 0);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA, data);

	/* mmSPI_CONFIG_CNTL_REMAP -> mmSPI_CONFIG_CNTL */
	data = (SOC15_REG_OFFSET(GC, 0, mmSPI_CONFIG_CNTL_REMAP) <<
		GRBM_CAM_DATA__CAM_ADDR__SHIFT) |
	       (SOC15_REG_OFFSET(GC, 0, mmSPI_CONFIG_CNTL) <<
		GRBM_CAM_DATA__CAM_REMAPADDR__SHIFT);

	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA_UPPER, 0);
	WREG32_SOC15(GC, 0, mmGRBM_CAM_DATA, data);
}

static u32 gfx_v10_0_get_cu_active_bitmap_per_sh(struct amdgpu_device *adev)
{
	u32 wgp_idx, wgp_active_bitmap;
	u32 cu_bitmap_per_wgp, cu_active_bitmap;

	wgp_active_bitmap = gfx_v10_0_get_wgp_active_bitmap_per_sh(adev);
	cu_active_bitmap = 0;

	for (wgp_idx = 0; wgp_idx < 16; wgp_idx++) {
		/* if there is one WGP enabled,
		 * it means 2 CUs will be enabled
		 */
		cu_bitmap_per_wgp = 3 << (2 * wgp_idx);
		if (wgp_active_bitmap & (1 << wgp_idx))
			cu_active_bitmap |= cu_bitmap_per_wgp;
	}

	return cu_active_bitmap;
}

static void
gfx_v10_0_set_user_wgp_inactive_bitmap_per_sh(struct amdgpu_device *adev,
					      u32 bitmap)
{
	u32 data;

	if (!bitmap)
		return;

	data = bitmap << GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT;
	data &= GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK;

	WREG32_SOC15(GC, 0, mmGC_USER_SHADER_ARRAY_CONFIG, data);
}

static int gfx_v10_0_downgrade_setting(struct amdgpu_device *adev)
{
	gfx_v10_0_reconf_se(adev);
	return 0;
}

static int gfx_v10_0_hw_init(void *handle)
{
	int r;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	if (adev->asic_type != CHIP_VANGOGH_LITE) {
		DRM_ERROR("Unsupported ASIC type - shouldn't be here\n");
		return -ENOTSUPP;
	}

	gfx_v10_0_init_golden_registers(adev);

	/* if GRBM CAM not remapped, set up the remapping */
	/* From HW team information these GRBM cam mapping are not needed
	 * anymore for Mariner. These registers got moved to un-privilege
	 * range so these registers are not needed anymore.
	 *	D: mmVGT_HS_OFFCHIP_PARAM_UMD
	 *	D: mmVGT_TF_MEMORY_BASE_UMD
	 *	D: mmVGT_TF_RING_SIZE_UMD
	 *	D: mmVGT_ESGS_RING_SIZE_UMD
	 *	D: mmSPI_CONFIG_CNTL_REMAP
	 *	D: mmVGT_TF_MEMORY_BASE_HI_UMD
	 *	D: mmVGT_GSVS_RING_SIZE_UMD
	 * otherwise, some related cts will fail, such as tesslation
	 * */
	if ((adev->asic_type != CHIP_VANGOGH_LITE) &&
		!gfx_v10_0_check_grbm_cam_remapping(adev))
		gfx_v10_0_setup_grbm_cam_remapping(adev);

	if (amdgpu_eval_mode != 0)
		gfx_v10_0_downgrade_setting(adev);

	gfx_v10_0_constants_init(adev);

	gfx_v10_0_init_csb(adev);

	/*
	 * init golden registers and rlc resume may override some registers,
	 * reconfig them here
	 */
	gfx_v10_0_tcp_harvest(adev);

	r = gfx_v10_0_cp_resume(adev);
	if (r)
		return r;

	if (AMDGPU_IS_MGFX0_EVT0(adev->grbm_chip_rev))
		vangogh_lite_gc_clock_gating_workaround(adev);

	return r;
}

#ifndef BRING_UP_DEBUG
static int gfx_v10_0_kiq_disable_kgq(struct amdgpu_device *adev)
{
	struct amdgpu_kiq *kiq = &adev->gfx.kiq;
	struct amdgpu_ring *kiq_ring = &kiq->ring;
	int i;

	if (!kiq->pmf || !kiq->pmf->kiq_unmap_queues)
		return -EINVAL;

	if (amdgpu_ring_alloc(kiq_ring, kiq->pmf->unmap_queues_size *
				adev->gfx.num_gfx_rings))
		return -ENOMEM;

	for (i = 0; i < adev->gfx.num_gfx_rings; i++)
		kiq->pmf->kiq_unmap_queues(kiq_ring, &adev->gfx.gfx_ring[i],
					   PREEMPT_QUEUES, 0, 0);
	if (!adev->job_hang)
		return amdgpu_ring_test_helper(kiq_ring);
	else
		return 0;
}
#endif

static int gfx_v10_0_hw_fini(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int r;
	uint32_t tmp;

	amdgpu_irq_put(adev, &adev->gfx.priv_reg_irq, 0);
	amdgpu_irq_put(adev, &adev->gfx.priv_inst_irq, 0);

	if (!sgpu_no_hw_access) {
#ifndef BRING_UP_DEBUG
		if (amdgpu_async_gfx_ring &&
		    adev->asic_type != CHIP_VANGOGH_LITE) {
			r = gfx_v10_0_kiq_disable_kgq(adev);
			if (r)
				DRM_ERROR("KGQ disable failed\n");
		}
#endif
		if (amdgpu_gfx_disable_kcq(adev))
			DRM_ERROR("KCQ disable failed\n");
	}

	if (amdgpu_sriov_vf(adev)) {
		gfx_v10_0_cp_gfx_enable(adev, false);
		/* Program KIQ position of RLC_CP_SCHEDULERS during destroy */
		tmp = RREG32_SOC15(GC, 0, mmRLC_CP_SCHEDULERS);
		tmp &= 0xffffff00;
		WREG32_SOC15(GC, 0, mmRLC_CP_SCHEDULERS, tmp);

		return 0;
	}
	gfx_v10_0_cp_enable(adev, false);
	gfx_v10_0_enable_gui_idle_interrupt(adev, false);

	return 0;
}

static int gfx_v10_0_suspend(void *handle)
{
	return gfx_v10_0_hw_fini(handle);
}

static int gfx_v10_0_resume(void *handle)
{
	return gfx_v10_0_hw_init(handle);
}

static bool gfx_v10_0_is_idle(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	if (REG_GET_FIELD(RREG32_SOC15(GC, 0, mmGRBM_STATUS),
				GRBM_STATUS, GUI_ACTIVE))
		return false;
	else
		return true;
}

static int gfx_v10_0_wait_for_idle(void *handle)
{
	unsigned i;
	u32 tmp;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	for (i = 0; i < adev->usec_timeout; i++) {
		/* read MC_STATUS */
		tmp = RREG32_SOC15(GC, 0, mmGRBM_STATUS) &
			GRBM_STATUS__GUI_ACTIVE_MASK;

		if (!REG_GET_FIELD(tmp, GRBM_STATUS, GUI_ACTIVE))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static bool gfx_v10_0_check_soft_reset(void *handle)
{
	uint32_t tmp;
	bool soft_reset = false;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	tmp = RREG32_SOC15(GC, 0, mmGRBM_STATUS);
	if (tmp & (GRBM_STATUS__PA_BUSY_MASK | GRBM_STATUS__SC_BUSY_MASK |
		   GRBM_STATUS__BCI_BUSY_MASK | GRBM_STATUS__SX_BUSY_MASK |
		   GRBM_STATUS__TA_BUSY_MASK | GRBM_STATUS__DB_BUSY_MASK |
		   GRBM_STATUS__CB_BUSY_MASK | GRBM_STATUS__GDS_BUSY_MASK |
		   GRBM_STATUS__SPI_BUSY_MASK | GRBM_STATUS__GE_BUSY_NO_DMA_MASK
		   | GRBM_STATUS__BCI_BUSY_MASK | GRBM_STATUS__CP_BUSY_MASK
		   | GRBM_STATUS__CP_COHERENCY_BUSY_MASK))
		soft_reset = true;

	tmp = RREG32_SOC15(GC, 0, mmGRBM_STATUS2);
	if (REG_GET_FIELD(tmp, GRBM_STATUS2, RLC_BUSY))
		soft_reset = true;

	return soft_reset;
}

static int gfx_v10_0_pre_soft_reset(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	DRM_INFO("IP block: gfx_v10_0 soft reset begin!\n");

	/* Software workaround for M0 only.  When compute HQD
	 * registers are reset through CP_SOFT_RESET_CNTL, there
	 * is a mismatch in cyces due to sclk_cmp is not active
	 * during soft reset.  This is fixed for M1.
	 * The workaround is to disable clock gating before soft
	 * reset and re-enable after.
	 */
	if (adev->asic_type == CHIP_VANGOGH_LITE &&
		AMDGPU_IS_MGFX0(adev->grbm_chip_rev))
		gfx_v10_0_set_clockgating_state(handle, AMD_CG_STATE_UNGATE);

	/* Disable GFX & MEC parsing/prefetching */
	gfx_v10_0_cp_enable(adev, false);

	return 0;
}

static int gfx_v10_0_soft_reset(void *handle)
{
	u32 grbm_soft_reset = 0;
	u32 tmp;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	/* GRBM_STATUS */
	tmp = RREG32_SOC15(GC, 0, mmGRBM_STATUS);
	if (tmp & (GRBM_STATUS__PA_BUSY_MASK | GRBM_STATUS__SC_BUSY_MASK |
		   GRBM_STATUS__BCI_BUSY_MASK | GRBM_STATUS__SX_BUSY_MASK |
		   GRBM_STATUS__TA_BUSY_MASK | GRBM_STATUS__DB_BUSY_MASK |
		   GRBM_STATUS__CB_BUSY_MASK | GRBM_STATUS__GDS_BUSY_MASK |
		   GRBM_STATUS__SPI_BUSY_MASK | GRBM_STATUS__GE_BUSY_NO_DMA_MASK |
		   GRBM_STATUS__BCI_BUSY_MASK | GRBM_STATUS__CP_BUSY_MASK |
		   GRBM_STATUS__CP_COHERENCY_BUSY_MASK)) {
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset,
						GRBM_SOFT_RESET, SOFT_RESET_CP,
						1);
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset,
						GRBM_SOFT_RESET, SOFT_RESET_GFX,
						1);
	}

	/* GRBM_STATUS2 */
	tmp = RREG32_SOC15(GC, 0, mmGRBM_STATUS2);
	if (REG_GET_FIELD(tmp, GRBM_STATUS2, RLC_BUSY))
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset,
						GRBM_SOFT_RESET,
						SOFT_RESET_RLC,
						1);

	if (grbm_soft_reset) {
		/* Enter RLC safe mode */
		gfx_v10_0_set_safe_mode(adev);

		/* reset all of the CP_HQD* registers to default value */
		tmp = RREG32_SOC15(GC, 0, mmCP_SOFT_RESET_CNTL);
		tmp |= CP_SOFT_RESET_CNTL__CMP_HQD_REG_RESET_MASK;
		WREG32_SOC15(GC, 0, mmCP_SOFT_RESET_CNTL, tmp);

		udelay(50);

		tmp = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);
		tmp |= grbm_soft_reset;
		dev_info(adev->dev, "GRBM_SOFT_RESET=0x%08X\n", tmp);
		WREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET, tmp);
		tmp = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);

		udelay(50);

		tmp &= ~grbm_soft_reset;
		WREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET, tmp);
		tmp = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);

		udelay(50);

		/* Exit RLC safe mode */
		gfx_v10_0_unset_safe_mode(adev);

		/* Wait a little for things to settle down */
		udelay(50);
	}
	return 0;
}

static int gfx_v10_0_post_soft_reset(void *handle)
{
	int r = -EINVAL;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	navi1x_soft_reset_SQG_workaround(adev);
	/* Resume CP if soft reset is successfull.*/
	if (!gfx_v10_0_check_soft_reset(adev)) {
		r = gfx_v10_0_soft_reset_cp_resume(adev);
		if (r)
			DRM_ERROR("failed to resume CP\n");
	}

	if (adev->asic_type == CHIP_VANGOGH_LITE &&
		AMDGPU_IS_MGFX0(adev->grbm_chip_rev))
		gfx_v10_0_set_clockgating_state(handle, AMD_CG_STATE_GATE);

	/*
	 * Making sure that we are in idle state.
	 */
	if (!r) {
		r = amdgpu_device_ip_wait_for_idle(adev, AMD_IP_BLOCK_TYPE_GFX);
		if (r)
			DRM_ERROR("failed waiting for idle\n");
	}

	DRM_INFO("IP block: gfx_v10_0 soft reset end!\n");

	return 0;
}

static uint64_t gfx_v10_0_get_gpu_clock_counter(struct amdgpu_device *adev)
{
	uint64_t clock;

	amdgpu_gfx_off_ctrl(adev, false);
	mutex_lock(&adev->gfx.gpu_clock_mutex);
	if (adev->asic_type == CHIP_VANGOGH_LITE) {
		/* Due to kernel upgrade to 5.10, it changed to use SMUIO to
		 * get the counter, but there is no SMU on Mariner, we still
		 * use RLC as before.
		 */
		WREG32_SOC15(GC, 0, mmRLC_CAPTURE_GPU_CLOCK_COUNT, 1);
		clock = (uint64_t)RREG32_SOC15(GC, 0, mmRLC_GPU_CLOCK_COUNT_LSB) |
			((uint64_t)RREG32_SOC15(GC, 0, mmRLC_GPU_CLOCK_COUNT_MSB) << 32ULL);
	} else {
		clock = (uint64_t)RREG32_SOC15(SMUIO, 0, mmGOLDEN_TSC_COUNT_LOWER) |
			((uint64_t)RREG32_SOC15(SMUIO, 0, mmGOLDEN_TSC_COUNT_UPPER) << 32ULL);
	}
	mutex_unlock(&adev->gfx.gpu_clock_mutex);
	amdgpu_gfx_off_ctrl(adev, true);
	return clock;
}

static void gfx_v10_0_ring_emit_gds_switch(struct amdgpu_ring *ring,
					   uint32_t vmid,
					   uint32_t gds_base, uint32_t gds_size,
					   uint32_t gws_base, uint32_t gws_size,
					   uint32_t oa_base, uint32_t oa_size)
{
	struct amdgpu_device *adev = ring->adev;

	if (adev->asic_type == CHIP_VANGOGH_LITE)
		return;

	/* GDS Base */
	gfx_v10_0_write_data_to_reg(ring, 0, false,
				    SOC15_REG_OFFSET(GC, 0, mmGDS_VMID0_BASE) + 2 * vmid,
				    gds_base);

	/* GDS Size */
	gfx_v10_0_write_data_to_reg(ring, 0, false,
				    SOC15_REG_OFFSET(GC, 0, mmGDS_VMID0_SIZE) + 2 * vmid,
				    gds_size);

	/* GWS */
	gfx_v10_0_write_data_to_reg(ring, 0, false,
				    SOC15_REG_OFFSET(GC, 0, mmGDS_GWS_VMID0) + vmid,
				    gws_size << GDS_GWS_VMID0__SIZE__SHIFT | gws_base);

	/* OA */
	gfx_v10_0_write_data_to_reg(ring, 0, false,
				    SOC15_REG_OFFSET(GC, 0, mmGDS_OA_VMID0) + vmid,
				    (1 << (oa_size + oa_base)) - (1 << oa_base));
}

static int gfx_v10_0_early_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int r;

	if (adev->asic_type != CHIP_VANGOGH_LITE) {
		DRM_ERROR("Unsupported ASIC type\n");
		return -ENOTSUPP;
	}

	adev->gfx.num_gfx_rings = 4;
	adev->gfx.num_compute_rings = 4;

	gfx_v10_0_set_ring_funcs(adev);
	gfx_v10_0_set_irq_funcs(adev);
	gfx_v10_0_set_gds_init(adev);
	gfx_v10_0_set_rlc_funcs(adev);

	/* allocate visible FB for rlc auto-loading fw */
	if (adev->firmware.load_type == AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO) {
		r = gfx_v10_0_rlc_backdoor_autoload_buffer_init(adev);
		if (r)
			return r;
	} else if (adev->firmware.load_type == AMDGPU_FW_LOAD_DIRECT) {
		r = gfx_v10_0_init_microcode(adev);
		if (r) {
			DRM_ERROR("Failed to load gfx firmware!\n");
			return r;
		}
	}
	return 0;
}

static int gfx_v10_0_late_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int r;

	if (!adev->irq.installed)
		return 0;

	r = amdgpu_irq_get(adev, &adev->gfx.priv_reg_irq, 0);
	if (r)
		return r;

	r = amdgpu_irq_get(adev, &adev->gfx.priv_inst_irq, 0);
	if (r)
		return r;

	return 0;
}

static bool gfx_v10_0_is_rlc_enabled(struct amdgpu_device *adev)
{
	uint32_t rlc_cntl;

	/* if RLC is not enabled, do nothing */
	rlc_cntl = RREG32_SOC15(GC, 0, mmRLC_CNTL);
	return (REG_GET_FIELD(rlc_cntl, RLC_CNTL, RLC_ENABLE_F32)) ? true : false;
}

static void gfx_v10_0_set_safe_mode(struct amdgpu_device *adev)
{
	uint32_t data;
	unsigned i;

	data = RLC_SAFE_MODE__CMD_MASK;
	data |= (1 << RLC_SAFE_MODE__MESSAGE__SHIFT);

	WREG32_SOC15(GC, 0, mmRLC_SAFE_MODE, data);

	/* wait for RLC_SAFE_MODE */
	for (i = 0; i < adev->usec_timeout; i++) {
		if (!REG_GET_FIELD(RREG32_SOC15(GC, 0, mmRLC_SAFE_MODE),
				   RLC_SAFE_MODE, CMD))
			break;
		udelay(1);
	}
}

static void gfx_v10_0_unset_safe_mode(struct amdgpu_device *adev)
{
	uint32_t data;

	data = RLC_SAFE_MODE__CMD_MASK;
	WREG32_SOC15(GC, 0, mmRLC_SAFE_MODE, data);
}

static void gfx_v10_0_enable_gfx_static_wgp_clock_gating(
				struct amdgpu_device *adev,
				bool enable)
{
	u32 i,j;
	uint32_t data, default_data;

	default_data = data = RREG32_SOC15(GC, 0, mmRLC_MAX_PG_WGP);
	data = REG_SET_FIELD(data, RLC_MAX_PG_WGP, MAX_POWERED_UP_WGP,
				adev->gfx.num_clock_on_wgp);
	if(default_data != data)
		WREG32(SOC15_REG_OFFSET(GC, 0, mmRLC_MAX_PG_WGP), data);

	mutex_lock(&adev->grbm_idx_mutex);
	for (i = 0; i < adev->gfx.config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx.config.max_sh_per_se; j++) {
			amdgpu_gfx_select_se_sh(adev, i, j, 0xffffffff);
			data = RREG32_SOC15(GC, 0, mmRLC_PG_ALWAYS_ON_WGP_MASK);
			if (data != adev->gfx.wgp_aon_bitmap[i][j])
				WREG32_SOC15(GC, 0, mmRLC_PG_ALWAYS_ON_WGP_MASK,
						adev->gfx.wgp_aon_bitmap[i][j]);
		}
	}
	amdgpu_gfx_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);
	mutex_unlock(&adev->grbm_idx_mutex);

	default_data = data = RREG32_SOC15(GC, 0, mmRLC_PG_CNTL);
	data = REG_SET_FIELD(data, RLC_PG_CNTL,
				STATIC_PER_WGP_PG_ENABLE,
				enable ? 1 : 0);
	if (default_data != data)
		WREG32_SOC15(GC, 0, mmRLC_PG_CNTL, data);
}

static void gfx_v10_0_update_medium_grain_clock_gating(struct amdgpu_device *adev,
						      bool enable)
{
	uint32_t data, def;

	if (!(adev->cg_flags & (AMD_CG_SUPPORT_GFX_MGCG | AMD_CG_SUPPORT_GFX_MGLS)))
		return;

	/* It is disabled by HW by default */
	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_GFX_MGCG)) {
		/* 0 - Disable some blocks' MGCG */
		WREG32_SOC15(GC, 0, mmGRBM_GFX_INDEX, 0xe0000000);
		WREG32_SOC15(GC, 0, mmCGTT_WD_CLK_CTRL, 0xff000000);
		WREG32_SOC15(GC, 0, mmCGTT_VGT_CLK_CTRL, 0xff000000);
		WREG32_SOC15(GC, 0, mmCGTT_IA_CLK_CTRL, 0xff000000);

		/* 1 - RLC_CGTT_MGCG_OVERRIDE */
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE);
		data &= ~(RLC_CGTT_MGCG_OVERRIDE__GRBM_CGTT_SCLK_OVERRIDE_MASK |
			  RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGCG_OVERRIDE_MASK |
			  RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGLS_OVERRIDE_MASK |
			  RLC_CGTT_MGCG_OVERRIDE__GFXIP_FGCG_OVERRIDE_MASK |
			  RLC_CGTT_MGCG_OVERRIDE__ENABLE_CGTS_LEGACY_MASK);

		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE, data);

		/* MGLS is a global flag to control all MGLS in GFX */
		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_MGLS) {
			/* 2 - RLC memory Light sleep */
			if (adev->cg_flags & AMD_CG_SUPPORT_GFX_RLC_LS) {
				def = data = RREG32_SOC15(GC, 0, mmRLC_MEM_SLP_CNTL);
				data |= RLC_MEM_SLP_CNTL__RLC_MEM_LS_EN_MASK;
				if (def != data)
					WREG32_SOC15(GC, 0, mmRLC_MEM_SLP_CNTL, data);
			}
			/* 3 - CP memory Light sleep */
			if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CP_LS) {
				def = data = RREG32_SOC15(GC, 0, mmCP_MEM_SLP_CNTL);
				data |= CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK;
				if (def != data)
					WREG32_SOC15(GC, 0, mmCP_MEM_SLP_CNTL, data);
			}
		}
	} else if (!enable || !(adev->cg_flags & AMD_CG_SUPPORT_GFX_MGCG)) {
		/* 1 - MGCG_OVERRIDE */
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE);
		data |= (RLC_CGTT_MGCG_OVERRIDE__RLC_CGTT_SCLK_OVERRIDE_MASK |
			 RLC_CGTT_MGCG_OVERRIDE__GRBM_CGTT_SCLK_OVERRIDE_MASK |
			 RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGCG_OVERRIDE_MASK |
			 RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGLS_OVERRIDE_MASK |
			 RLC_CGTT_MGCG_OVERRIDE__GFXIP_FGCG_OVERRIDE_MASK |
			 RLC_CGTT_MGCG_OVERRIDE__ENABLE_CGTS_LEGACY_MASK);
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE, data);

		/* 2 - disable MGLS in CP */
		data = RREG32_SOC15(GC, 0, mmCP_MEM_SLP_CNTL);
		if (data & CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK) {
			data &= ~CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK;
			WREG32_SOC15(GC, 0, mmCP_MEM_SLP_CNTL, data);
		}

		/* 3 - disable MGLS in RLC */
		data = RREG32_SOC15(GC, 0, mmRLC_MEM_SLP_CNTL);
		if (data & RLC_MEM_SLP_CNTL__RLC_MEM_LS_EN_MASK) {
			data &= ~RLC_MEM_SLP_CNTL__RLC_MEM_LS_EN_MASK;
			WREG32_SOC15(GC, 0, mmRLC_MEM_SLP_CNTL, data);
		}

	}
}

static void gfx_v10_0_update_3d_clock_gating(struct amdgpu_device *adev,
					   bool enable)
{
	uint32_t data, def;

	if (!(adev->cg_flags & (AMD_CG_SUPPORT_GFX_3D_CGCG | AMD_CG_SUPPORT_GFX_3D_CGLS)))
		return;

	/* Enable 3D CGCG/CGLS */
	if (enable) {
		/* write cmd to clear cgcg/cgls ov */
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE);

		/* unset CGCG override */
		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_3D_CGCG)
			data &= ~RLC_CGTT_MGCG_OVERRIDE__GFXIP_GFX3D_CG_OVERRIDE_MASK;

		/* update CGCG and CGLS override bits */
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE, data);

		/* enable 3Dcgcg FSM(0x0000363f) */
		def = RREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL_3D);
		data = 0;

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_3D_CGCG)
			data = (0x36 << RLC_CGCG_CGLS_CTRL_3D__CGCG_GFX_IDLE_THRESHOLD__SHIFT) |
				RLC_CGCG_CGLS_CTRL_3D__CGCG_EN_MASK;

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_3D_CGLS)
			data |= (0x000F << RLC_CGCG_CGLS_CTRL_3D__CGLS_REP_COMPANSAT_DELAY__SHIFT) |
				RLC_CGCG_CGLS_CTRL_3D__CGLS_EN_MASK;

		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL_3D, data);

		WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL_3D, 0x0001003d);

		/* set IDLE_POLL_COUNT(0x00900100) */
		def = RREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_CNTL);
		data = (0x0000 << CP_RB_WPTR_POLL_CNTL__POLL_FREQUENCY__SHIFT) |
			(0x0060 << CP_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT);
		if (def != data)
			WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_CNTL, data);
	} else {
		/* Disable CGCG/CGLS */
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL_3D);

		/* disable cgcg, cgls should be disabled */
		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_3D_CGCG)
			data &= ~RLC_CGCG_CGLS_CTRL_3D__CGCG_EN_MASK;

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_3D_CGLS)
			data &= ~RLC_CGCG_CGLS_CTRL_3D__CGLS_EN_MASK;

		/* disable cgcg and cgls in FSM */
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL_3D, data);
	}
}

static void gfx_v10_0_update_coarse_grain_clock_gating(struct amdgpu_device *adev,
						      bool enable)
{
	uint32_t def, data;

	if (!(adev->cg_flags & (AMD_CG_SUPPORT_GFX_CGCG | AMD_CG_SUPPORT_GFX_CGLS)))
		return;

	if (enable) {
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE);

		/* unset CGCG override */
		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGCG)
			data &= ~RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGCG_OVERRIDE_MASK;

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGLS)
			data &= ~RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGLS_OVERRIDE_MASK;

		/* update CGCG and CGLS override bits */
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE, data);

		/* enable cgcg FSM(0x0000363F) */
		def = RREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL);
		data = 0;

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGCG)
			data = (0x36 << RLC_CGCG_CGLS_CTRL__CGCG_GFX_IDLE_THRESHOLD__SHIFT) |
				RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK;

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGLS)
			data |= (0x000F << RLC_CGCG_CGLS_CTRL__CGLS_REP_COMPANSAT_DELAY__SHIFT) |
				RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK;

		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL, data);

		/* set IDLE_POLL_COUNT(0x00900100) */
		def = RREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_CNTL);
		data = (0x0100 << CP_RB_WPTR_POLL_CNTL__POLL_FREQUENCY__SHIFT) |
			(0x0090 << CP_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT);
		if (def != data)
			WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_CNTL, data);
	} else {
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL);

		/* reset CGCG/CGLS bits */
		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGCG)
			data &= ~RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK;

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGLS)
			data &= ~RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK;

		/* disable cgcg and cgls in FSM */
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL, data);
	}

	WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL, 0x0001003d);
}

static void gfx_v10_0_update_gfx_static_wgp_clock_gating(
				struct amdgpu_device* adev,
				bool enable)
{
	if ((adev->cg_flags & AMD_CG_SUPPORT_GFX_STATIC_WGP) && enable)
		gfx_v10_0_enable_gfx_static_wgp_clock_gating(adev, true);
	else
		gfx_v10_0_enable_gfx_static_wgp_clock_gating(adev, false);
}

static void gfx_v10_0_update_fine_grain_clock_gating(struct amdgpu_device *adev,
						      bool enable)
{
	uint32_t def, data;

	if (!(adev->cg_flags & AMD_CG_SUPPORT_GFX_FGCG))
		return;

	if (enable) {
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE);
		/* unset FGCG override */
		data &= ~RLC_CGTT_MGCG_OVERRIDE__GFXIP_FGCG_OVERRIDE_MASK;
		/* update FGCG override bits */
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE, data);

		def = data = RREG32_SOC15(GC, 0, mmRLC_CLK_CNTL);
		/* unset RLC SRAM CLK GATER override */
		data &= ~RLC_CLK_CNTL__RLC_SRAM_CLK_GATER_OVERRIDE_MASK;
		/* update RLC SRAM CLK GATER override bits */
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CLK_CNTL, data);
	} else {
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE);
		/* reset FGCG bits */
		data |= RLC_CGTT_MGCG_OVERRIDE__GFXIP_FGCG_OVERRIDE_MASK;
		/* disable FGCG*/
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE, data);

		def = data = RREG32_SOC15(GC, 0, mmRLC_CLK_CNTL);
		/* reset RLC SRAM CLK GATER bits */
		data |= RLC_CLK_CNTL__RLC_SRAM_CLK_GATER_OVERRIDE_MASK;
		/* disable RLC SRAM CLK*/
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CLK_CNTL, data);
	}
}

static int gfx_v10_0_update_gfx_clock_gating(struct amdgpu_device *adev,
					    bool enable)
{
	if (adev->asic_type == CHIP_VANGOGH_LITE &&
		adev->cg_flags &
		(AMD_CG_SUPPORT_GFX_MGCG |
		 AMD_CG_SUPPORT_GFX_CGCG |
		 AMD_CG_SUPPORT_GFX_3D_CGCG))
		WREG32_SOC15(GC, 0, mmRLC_GPM_INT_DISABLE_TH0, 0);

	amdgpu_gfx_rlc_enter_safe_mode(adev);

	if (enable) {
		/* enable FGCG firstly*/
		gfx_v10_0_update_fine_grain_clock_gating(adev, enable);
		/* CGCG/CGLS should be enabled after MGCG/MGLS
		 * ===  MGCG + MGLS ===
		 */
		if (adev->asic_type == CHIP_VANGOGH_LITE)
			vangogh_lite_gc_update_median_grain_clock_gating(adev, enable);
		else
			gfx_v10_0_update_medium_grain_clock_gating(adev, enable);
		/* ===  CGCG /CGLS for GFX 3D Only === */
		gfx_v10_0_update_3d_clock_gating(adev, enable);
		/* ===  CGCG + CGLS === */
		gfx_v10_0_update_coarse_grain_clock_gating(adev, enable);
		/* static wgp CG state */
		gfx_v10_0_update_gfx_static_wgp_clock_gating(adev, enable);
	} else {
		/* static wgp CG state */
		gfx_v10_0_update_gfx_static_wgp_clock_gating(adev, enable);
		/* CGCG/CGLS should be disabled before MGCG/MGLS
		 * ===  CGCG + CGLS ===
		 */
		gfx_v10_0_update_coarse_grain_clock_gating(adev, enable);
		/* ===  CGCG /CGLS for GFX 3D Only === */
		gfx_v10_0_update_3d_clock_gating(adev, enable);
		/* ===  MGCG + MGLS === */
		if (adev->asic_type == CHIP_VANGOGH_LITE)
			vangogh_lite_gc_update_median_grain_clock_gating(adev, enable);
		else
			gfx_v10_0_update_medium_grain_clock_gating(adev, enable);
		/* disable fgcg at last*/
		gfx_v10_0_update_fine_grain_clock_gating(adev, enable);
	}

	if (adev->cg_flags &
	    (AMD_CG_SUPPORT_GFX_MGCG |
	     AMD_CG_SUPPORT_GFX_CGLS |
	     AMD_CG_SUPPORT_GFX_CGCG |
	     AMD_CG_SUPPORT_GFX_3D_CGCG |
	     AMD_CG_SUPPORT_GFX_3D_CGLS))
		gfx_v10_0_enable_gui_idle_interrupt(adev, enable);

	amdgpu_gfx_rlc_exit_safe_mode(adev);

	return 0;
}

static void gfx_v10_0_update_spm_vmid(struct amdgpu_device *adev, unsigned vmid)
{
	u32 reg, data;

	reg = SOC15_REG_OFFSET(GC, 0, mmRLC_SPM_MC_CNTL);
	if (amdgpu_sriov_is_pp_one_vf(adev))
		data = RREG32_NO_KIQ(reg);
	else
		data = RREG32(reg);

	data &= ~RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK;
	data |= (vmid & RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK) << RLC_SPM_MC_CNTL__RLC_SPM_VMID__SHIFT;

	if (amdgpu_sriov_is_pp_one_vf(adev))
		WREG32_SOC15_NO_KIQ(GC, 0, mmRLC_SPM_MC_CNTL, data);
	else
		WREG32_SOC15(GC, 0, mmRLC_SPM_MC_CNTL, data);
}

static bool gfx_v10_0_check_rlcg_range(struct amdgpu_device *adev,
					uint32_t offset,
					struct soc15_reg_rlcg *entries, int arr_size)
{
	int i;
	uint32_t reg;

	if (!entries)
		return false;

	for (i = 0; i < arr_size; i++) {
		const struct soc15_reg_rlcg *entry;

		entry = &entries[i];
		reg = adev->reg_offset[entry->hwip][entry->instance][entry->segment] + entry->reg;
		if (offset == reg)
			return true;
	}

	return false;
}

static bool gfx_v10_0_is_rlcg_access_range(struct amdgpu_device *adev, u32 offset)
{
	return gfx_v10_0_check_rlcg_range(adev, offset, NULL, 0);
}

static int gfx_v10_0_update_gfx_CGCG(struct amdgpu_device *adev,
				      bool enable)
{
	if (adev->asic_type == CHIP_VANGOGH_LITE &&
		adev->cg_flags &
		(AMD_CG_SUPPORT_GFX_CGCG |
		 AMD_CG_SUPPORT_GFX_3D_CGCG))
		WREG32_SOC15(GC, 0, mmRLC_GPM_INT_DISABLE_TH0, 0);

	amdgpu_gfx_rlc_enter_safe_mode(adev);

	if (enable) {
		gfx_v10_0_update_3d_clock_gating(adev, enable);
		gfx_v10_0_update_coarse_grain_clock_gating(adev, enable);
	} else {
		gfx_v10_0_update_coarse_grain_clock_gating(adev, enable);
		gfx_v10_0_update_3d_clock_gating(adev, enable);
	}

	if (adev->cg_flags &
	    (AMD_CG_SUPPORT_GFX_CGCG |
	     AMD_CG_SUPPORT_GFX_3D_CGCG))
		gfx_v10_0_enable_gui_idle_interrupt(adev, enable);

	amdgpu_gfx_rlc_exit_safe_mode(adev);
	return 0;
}

static const struct amdgpu_rlc_funcs gfx_v10_0_rlc_funcs = {
	.is_rlc_enabled = gfx_v10_0_is_rlc_enabled,
	.set_safe_mode = gfx_v10_0_set_safe_mode,
	.unset_safe_mode = gfx_v10_0_unset_safe_mode,
	.init = gfx_v10_0_rlc_init,
	.get_csb_size = gfx_v10_0_get_csb_size,
	.get_csb_buffer = gfx_v10_0_get_csb_buffer,
	.resume = gfx_v10_0_rlc_resume,
	.stop = gfx_v10_0_rlc_stop,
	.reset = gfx_v10_0_rlc_reset,
	.start = gfx_v10_0_rlc_start,
	.update_spm_vmid = gfx_v10_0_update_spm_vmid,
};

static const struct amdgpu_rlc_funcs gfx_v10_0_rlc_funcs_sriov = {
	.is_rlc_enabled = gfx_v10_0_is_rlc_enabled,
	.set_safe_mode = gfx_v10_0_set_safe_mode,
	.unset_safe_mode = gfx_v10_0_unset_safe_mode,
	.init = gfx_v10_0_rlc_init,
	.get_csb_size = gfx_v10_0_get_csb_size,
	.get_csb_buffer = gfx_v10_0_get_csb_buffer,
	.resume = gfx_v10_0_rlc_resume,
	.stop = gfx_v10_0_rlc_stop,
	.reset = gfx_v10_0_rlc_reset,
	.start = gfx_v10_0_rlc_start,
	.update_spm_vmid = gfx_v10_0_update_spm_vmid,
	.rlcg_wreg = gfx_v10_rlcg_wreg,
	.is_rlcg_access_range = gfx_v10_0_is_rlcg_access_range,
};

static int gfx_v10_0_set_powergating_state(void *handle,
					  enum amd_powergating_state state)
{
	/* Not supported by Vangogh. TODO: Remove */
	return 0;
}

static int gfx_v10_0_set_clockgating_state(void *handle,
					  enum amd_clockgating_state state)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	if (amdgpu_sriov_vf(adev))
		return 0;

	switch (adev->asic_type) {
	case CHIP_VANGOGH_LITE:
		gfx_v10_0_update_gfx_clock_gating(adev,
						 state == AMD_CG_STATE_GATE);
		break;
	default:
		break;
	}
	return 0;
}

static void gfx_v10_0_get_clockgating_state(void *handle, u32 *flags)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	int data;

	/* AMD_CG_SUPPORT_GFX_FGCG */
	data = RREG32_KIQ(SOC15_REG_OFFSET(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE));
	if (!(data & RLC_CGTT_MGCG_OVERRIDE__GFXIP_FGCG_OVERRIDE_MASK))
		*flags |= AMD_CG_SUPPORT_GFX_FGCG;

	/* AMD_CG_SUPPORT_GFX_MGCG */
	data = RREG32_KIQ(SOC15_REG_OFFSET(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE));
	if (!(data & RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGCG_OVERRIDE_MASK))
		*flags |= AMD_CG_SUPPORT_GFX_MGCG;

	/* AMD_CG_SUPPORT_GFX_CGCG */
	data = RREG32_KIQ(SOC15_REG_OFFSET(GC, 0, mmRLC_CGCG_CGLS_CTRL));
	if (data & RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK)
		*flags |= AMD_CG_SUPPORT_GFX_CGCG;

	/* AMD_CG_SUPPORT_GFX_CGLS */
	if (data & RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK)
		*flags |= AMD_CG_SUPPORT_GFX_CGLS;

	/* AMD_CG_SUPPORT_GFX_RLC_LS */
	data = RREG32_KIQ(SOC15_REG_OFFSET(GC, 0, mmRLC_MEM_SLP_CNTL));
	if (data & RLC_MEM_SLP_CNTL__RLC_MEM_LS_EN_MASK)
		*flags |= AMD_CG_SUPPORT_GFX_RLC_LS | AMD_CG_SUPPORT_GFX_MGLS;

	/* AMD_CG_SUPPORT_GFX_CP_LS */
	data = RREG32_KIQ(SOC15_REG_OFFSET(GC, 0, mmCP_MEM_SLP_CNTL));
	if (data & CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK)
		*flags |= AMD_CG_SUPPORT_GFX_CP_LS | AMD_CG_SUPPORT_GFX_MGLS;

	/* AMD_CG_SUPPORT_GFX_3D_CGCG */
	data = RREG32_KIQ(SOC15_REG_OFFSET(GC, 0, mmRLC_CGCG_CGLS_CTRL_3D));
	if (data & RLC_CGCG_CGLS_CTRL_3D__CGCG_EN_MASK)
		*flags |= AMD_CG_SUPPORT_GFX_3D_CGCG;

	/* AMD_CG_SUPPORT_GFX_3D_CGLS */
	if (data & RLC_CGCG_CGLS_CTRL_3D__CGLS_EN_MASK)
		*flags |= AMD_CG_SUPPORT_GFX_3D_CGLS;
}

#if defined(CONFIG_DRM_AMDGPU_DUMP)
static bool gfx_v10_0_is_power_on(void *handle)
{
	bool r = true;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	if (adev->pm.pp_feature & PP_GFXOFF_MASK) {
		u32 rreg32 = RREG32_SOC15(MP1, 0, mmMP1_SMN_EXT_SCRATCH0);

		if (((rreg32 & MP1_SMN_EXT_SCRATCH0__PWR_GFXOFF_STATUS_MASK) >>
			MP1_SMN_EXT_SCRATCH0__PWR_GFXOFF_STATUS__SHIFT) != 2)
			r = false;
	}

	return r;
}
#endif /* CONFIG_DRM_AMDGPU_DUMP */

static u64 gfx_v10_0_ring_get_rptr_gfx(struct amdgpu_ring *ring)
{
	return ring->adev->wb.wb[ring->rptr_offs]; /* gfx10 is 32bit rptr*/
}

static u64 gfx_v10_0_ring_get_rreg_gfx(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t reg_val = 0;

	mutex_lock(&adev->srbm_mutex);
	nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
	reg_val = RREG32_SOC15(GC, 0, mmCP_RB0_RPTR);
	mutex_unlock(&adev->srbm_mutex);

	return reg_val;
}

static u64 gfx_v10_0_ring_get_wptr_gfx(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	u64 wptr;

	/* XXX check if swapping is necessary on BE */
	if (ring->use_doorbell) {
		wptr = atomic64_read((atomic64_t *)&adev->wb.wb[ring->wptr_offs]);
	} else {
		wptr = RREG32_SOC15(GC, 0, mmCP_RB0_WPTR);
		wptr += (u64)RREG32_SOC15(GC, 0, mmCP_RB0_WPTR_HI) << 32;
	}

	return wptr;
}

static void gfx_v10_0_ring_set_wptr_gfx(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (ring->use_doorbell) {
		/* XXX check if swapping is necessary on BE */
		atomic64_set((atomic64_t *)&adev->wb.wb[ring->wptr_offs], ring->wptr);
		WDOORBELL64(ring->doorbell_index, ring->wptr);
	} else {
		WREG32_SOC15(GC, 0, mmCP_RB0_WPTR, lower_32_bits(ring->wptr));
		WREG32_SOC15(GC, 0, mmCP_RB0_WPTR_HI, upper_32_bits(ring->wptr));
	}
	SGPU_LOG(adev, DMSG_INFO, DMSG_POWER, "ring[%d] wptr:%x, %lx",
		ring->wptr_offs, ring->wptr, RREG32_SOC15(GC, 0, mmCP_DEBUG_CNTL));
}

static u64 gfx_v10_0_ring_get_rptr_compute(struct amdgpu_ring *ring)
{
	if (ring->cwsr || ring->tmz)
		return *ring->rptr_cpu_addr;

	/* gfx10 hardware is 32bit rptr */
	return ring->adev->wb.wb[ring->rptr_offs];
}

static u64 gfx_v10_0_ring_get_rreg_compute(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t reg_val = 0;

	mutex_lock(&adev->srbm_mutex);
	nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
	reg_val = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR);
	mutex_unlock(&adev->srbm_mutex);

	return reg_val;
}


static u64 gfx_v10_0_ring_get_wptr_compute(struct amdgpu_ring *ring)
{
	u64 wptr;

	if (ring->cwsr || ring->tmz) {
		wptr = atomic64_read((atomic64_t *)ring->wptr_cpu_addr);
		return wptr;
	}

	/* XXX check if swapping is necessary on BE */
	if (ring->use_doorbell)
		wptr = atomic64_read((atomic64_t *)&ring->adev->wb.wb[ring->wptr_offs]);
	else
		BUG();
	return wptr;
}

static void gfx_v10_0_ring_set_wptr_compute(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (ring->cwsr | ring->tmz) {
		atomic64_set((atomic64_t *)ring->wptr_cpu_addr,
			     ring->wptr);
		WDOORBELL64(ring->doorbell_index, ring->wptr);
		return;
	}
	/* XXX check if swapping is necessary on BE */
	if (ring->use_doorbell) {
		atomic64_set((atomic64_t *)&adev->wb.wb[ring->wptr_offs], ring->wptr);
		WDOORBELL64(ring->doorbell_index, ring->wptr);
	} else {
		BUG(); /* only DOORBELL method supported on gfx10 now */
	}
	SGPU_LOG(adev, DMSG_INFO, DMSG_POWER, "ring[%d] wptr:%x, %lx",
		ring->wptr_offs, ring->wptr, RREG32_SOC15(GC, 0, mmCP_DEBUG_CNTL));
}

static void gfx_v10_0_ring_emit_hdp_flush(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	u32 ref_and_mask, reg_mem_engine;
	const struct nbio_hdp_flush_reg *nbio_hf_reg = adev->nbio.hdp_flush_reg;

	if (ring->adev->asic_type == CHIP_VANGOGH_LITE)
		return;

	if (ring->funcs->type == AMDGPU_RING_TYPE_COMPUTE) {
		switch (ring->me) {
		case 1:
			ref_and_mask = nbio_hf_reg->ref_and_mask_cp2 << ring->pipe;
			break;
		case 2:
			ref_and_mask = nbio_hf_reg->ref_and_mask_cp6 << ring->pipe;
			break;
		default:
			return;
		}
		reg_mem_engine = 0;
	} else {
		ref_and_mask = nbio_hf_reg->ref_and_mask_cp0;
		reg_mem_engine = 1; /* pfp */
	}

	gfx_v10_0_wait_reg_mem(ring, reg_mem_engine, 0, 1,
			       adev->nbio.funcs->get_hdp_flush_req_offset(adev),
			       adev->nbio.funcs->get_hdp_flush_done_offset(adev),
			       ref_and_mask, ref_and_mask, 0x20);
}

static void gfx_v10_0_ring_emit_ib_gfx(struct amdgpu_ring *ring,
				       struct amdgpu_job *job,
				       struct amdgpu_ib *ib,
				       uint32_t flags)
{
	unsigned vmid = AMDGPU_JOB_GET_VMID(job);
	u32 header, control = 0;

	if (ib->flags & AMDGPU_IB_FLAG_CE)
		header = PACKET3(PACKET3_INDIRECT_BUFFER_CNST, 2);
	else
		header = PACKET3(PACKET3_INDIRECT_BUFFER, 2);

	control |= ib->length_dw | (vmid << 24);

	if ((amdgpu_sriov_vf(ring->adev) || amdgpu_mcbp) && (ib->flags & AMDGPU_IB_FLAG_PREEMPT)) {
		control |= INDIRECT_BUFFER_PRE_ENB(1);

		if (flags & AMDGPU_IB_PREEMPTED)
			control |= INDIRECT_BUFFER_PRE_RESUME(1);

		if (!(ib->flags & AMDGPU_IB_FLAG_CE) && vmid)
			gfx_v10_0_ring_emit_de_meta(ring,
				    (!amdgpu_sriov_vf(ring->adev) && flags & AMDGPU_IB_PREEMPTED) ? true : false);
	}

	amdgpu_ring_write(ring, header);
	BUG_ON(ib->gpu_addr & 0x3); /* Dword align */
	amdgpu_ring_write(ring,
#ifdef __BIG_ENDIAN
		(2 << 0) |
#endif
		lower_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, control);
}

static void gfx_v10_0_ring_emit_ib_compute(struct amdgpu_ring *ring,
					   struct amdgpu_job *job,
					   struct amdgpu_ib *ib,
					   uint32_t flags)
{
	unsigned vmid = AMDGPU_JOB_GET_VMID(job);
	u32 control = INDIRECT_BUFFER_VALID | ib->length_dw | (vmid << 24);

	/* Currently, there is a high possibility to get wave ID mismatch
	 * between ME and GDS, leading to a hw deadlock, because ME generates
	 * different wave IDs than the GDS expects. This situation happens
	 * randomly when at least 5 compute pipes use GDS ordered append.
	 * The wave IDs generated by ME are also wrong after suspend/resume.
	 * Those are probably bugs somewhere else in the kernel driver.
	 *
	 * Writing GDS_COMPUTE_MAX_WAVE_ID resets wave ID counters in ME and
	 * GDS to 0 for this ring (me/pipe).
	 */
	if (ib->flags & AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID) {
		amdgpu_ring_write(ring, PACKET3(PACKET3_SET_CONFIG_REG, 1));
		amdgpu_ring_write(ring, mmGDS_COMPUTE_MAX_WAVE_ID);
		amdgpu_ring_write(ring, ring->adev->gds.gds_compute_max_wave_id);
	}

	amdgpu_ring_write(ring, PACKET3(PACKET3_INDIRECT_BUFFER, 2));
	BUG_ON(ib->gpu_addr & 0x3); /* Dword align */
	amdgpu_ring_write(ring,
#ifdef __BIG_ENDIAN
				(2 << 0) |
#endif
				lower_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, control);
}

static void gfx_v10_0_ring_emit_fence(struct amdgpu_ring *ring, u64 addr,
				     u64 seq, unsigned flags)
{
	bool write64bit = flags & AMDGPU_FENCE_FLAG_64BIT;
	bool int_sel = flags & AMDGPU_FENCE_FLAG_INT;

	/* RELEASE_MEM - flush caches, send int */
	amdgpu_ring_write(ring, PACKET3(PACKET3_RELEASE_MEM, 6));
	amdgpu_ring_write(ring, (PACKET3_RELEASE_MEM_GCR_SEQ |
				 PACKET3_RELEASE_MEM_GCR_GL2_WB |
				 PACKET3_RELEASE_MEM_GCR_GLM_INV |
				 PACKET3_RELEASE_MEM_CACHE_POLICY(3) |
				 PACKET3_RELEASE_MEM_EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) |
				 PACKET3_RELEASE_MEM_EVENT_INDEX(5)));
	amdgpu_ring_write(ring, (PACKET3_RELEASE_MEM_DATA_SEL(write64bit ? 2 : 1) |
				 PACKET3_RELEASE_MEM_INT_SEL(int_sel ? 2 : 0)));

	/*
	 * the address should be Qword aligned if 64bit write, Dword
	 * aligned if only send 32bit data low (discard data high)
	 */
	if (write64bit)
		BUG_ON(addr & 0x7);
	else
		BUG_ON(addr & 0x3);
	amdgpu_ring_write(ring, lower_32_bits(addr));
	amdgpu_ring_write(ring, upper_32_bits(addr));
	amdgpu_ring_write(ring, lower_32_bits(seq));
	amdgpu_ring_write(ring, upper_32_bits(seq));

	/*
	 * Send gfx ring id via INT_CTXID field in interrupt payload.
	 * This is necessary for multiple gfx queues case because queues
	 * in same gfx pipe cannot be distinguished by queue_id in interrupt
	 * payload. Workaround before gfx firmware support this.
	 */
	if ((ring->adev->asic_type != CHIP_VANGOGH_LITE) && (ring->funcs->type == AMDGPU_RING_TYPE_GFX)) {
		uint32_t idx = ring - &ring->adev->gfx.gfx_ring[0];
		amdgpu_ring_write(ring, idx);
	} else {
		amdgpu_ring_write(ring, 0);
	}
}

static void gfx_v10_0_ring_emit_pipeline_sync(struct amdgpu_ring *ring)
{
	int usepfp = (ring->funcs->type == AMDGPU_RING_TYPE_GFX);
	uint32_t seq = ring->fence_drv.sync_seq;
	uint64_t addr = ring->fence_drv.gpu_addr;

	gfx_v10_0_wait_reg_mem(ring, usepfp, 1, 0, lower_32_bits(addr),
			       upper_32_bits(addr), seq, 0xffffffff, 4);
}

static void gfx_v10_0_ring_emit_vm_flush(struct amdgpu_ring *ring,
					 unsigned vmid, uint64_t pd_addr)
{
	amdgpu_gmc_emit_flush_gpu_tlb(ring, vmid, pd_addr);

	/* compute doesn't have PFP */
	if (ring->funcs->type == AMDGPU_RING_TYPE_GFX) {
		/* sync PFP to ME, otherwise we might get invalid PFP reads */
		amdgpu_ring_write(ring, PACKET3(PACKET3_PFP_SYNC_ME, 0));
		amdgpu_ring_write(ring, 0x0);
	}
}

static void gfx_v10_0_ring_emit_fence_kiq(struct amdgpu_ring *ring, u64 addr,
					  u64 seq, unsigned int flags)
{
	struct amdgpu_device *adev = ring->adev;

	/* we only allocate 32bit for each seq wb address */
	BUG_ON(flags & AMDGPU_FENCE_FLAG_64BIT);

	/* write fence seq to the "addr" */
	amdgpu_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
	amdgpu_ring_write(ring, (WRITE_DATA_ENGINE_SEL(0) |
				 WRITE_DATA_DST_SEL(5) | WR_CONFIRM));
	amdgpu_ring_write(ring, lower_32_bits(addr));
	amdgpu_ring_write(ring, upper_32_bits(addr));
	amdgpu_ring_write(ring, lower_32_bits(seq));

	if (flags & AMDGPU_FENCE_FLAG_INT) {
		/* set register to trigger INT */
		amdgpu_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
		amdgpu_ring_write(ring, (WRITE_DATA_ENGINE_SEL(0) |
					 WRITE_DATA_DST_SEL(0) | WR_CONFIRM));
		amdgpu_ring_write(ring, SOC15_REG_OFFSET(GC, 0, mmCPC_INT_STATUS));
		amdgpu_ring_write(ring, 0);
		amdgpu_ring_write(ring, 0x20000000); /* src_id is 178 */
	}
}

static void gfx_v10_0_ring_emit_sb(struct amdgpu_ring *ring)
{
	amdgpu_ring_write(ring, PACKET3(PACKET3_SWITCH_BUFFER, 0));
	amdgpu_ring_write(ring, 0);
}

static void gfx_v10_0_ring_emit_cntxcntl(struct amdgpu_ring *ring,
					 uint32_t flags)
{
	uint32_t dw2 = 0;

	if ((amdgpu_mcbp || amdgpu_sriov_vf(ring->adev)) &&
		ring->adev->asic_type != CHIP_VANGOGH_LITE)
		gfx_v10_0_ring_emit_ce_meta(ring,
				    (!amdgpu_sriov_vf(ring->adev) && flags & AMDGPU_IB_PREEMPTED) ? true : false);

	dw2 |= 0x80000000; /* set load_enable otherwise this package is just NOPs */
	if (flags & AMDGPU_HAVE_CTX_SWITCH) {
		/* set load_global_config & load_global_uconfig */
		dw2 |= 0x8001;
		/* set load_cs_sh_regs */
		dw2 |= 0x01000000;
		/* set load_per_context_state & load_gfx_sh_regs for GFX */
		dw2 |= 0x10002;

		/* set load_ce_ram if preamble presented */
		if (AMDGPU_PREAMBLE_IB_PRESENT & flags)
			dw2 |= 0x10000000;
	} else {
		/* still load_ce_ram if this is the first time preamble presented
		 * although there is no context switch happens.
		 */
		if (AMDGPU_PREAMBLE_IB_PRESENT_FIRST & flags)
			dw2 |= 0x10000000;
	}

	amdgpu_ring_write(ring, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	amdgpu_ring_write(ring, dw2);
	amdgpu_ring_write(ring, 0);
}

static unsigned gfx_v10_0_ring_emit_init_cond_exec(struct amdgpu_ring *ring)
{
	unsigned ret;

	amdgpu_ring_write(ring, PACKET3(PACKET3_COND_EXEC, 3));
	amdgpu_ring_write(ring, lower_32_bits(ring->cond_exe_gpu_addr));
	amdgpu_ring_write(ring, upper_32_bits(ring->cond_exe_gpu_addr));
	amdgpu_ring_write(ring, 0); /* discard following DWs if *cond_exec_gpu_addr==0 */
	ret = ring->wptr & ring->buf_mask;
	amdgpu_ring_write(ring, 0x55aa55aa); /* patch dummy value later */

	return ret;
}

static void gfx_v10_0_ring_emit_patch_cond_exec(struct amdgpu_ring *ring, unsigned offset)
{
	unsigned cur;
	BUG_ON(offset > ring->buf_mask);
	BUG_ON(ring->ring[offset] != 0x55aa55aa);

	cur = (ring->wptr - 1) & ring->buf_mask;
	if (likely(cur > offset))
		ring->ring[offset] = cur - offset;
	else
		ring->ring[offset] = (ring->buf_mask + 1) - offset + cur;
}

static int gfx_v10_0_ring_preempt_ib(struct amdgpu_ring *ring)
{
	int i, r = 0;
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_kiq *kiq = &adev->gfx.kiq;
	struct amdgpu_ring *kiq_ring = &kiq->ring;
	struct timespec64 start, end, duration;
	unsigned long flags;

	if (!kiq->pmf || !kiq->pmf->kiq_unmap_queues)
		return -EINVAL;

	spin_lock_irqsave(&kiq->ring_lock, flags);

	if (amdgpu_ring_alloc(kiq_ring, kiq->pmf->unmap_queues_size)) {
		spin_unlock_irqrestore(&kiq->ring_lock, flags);
		return -ENOMEM;
	}

	/* assert preemption condition */
	amdgpu_ring_set_preempt_cond_exec(ring, false);

	/* assert IB preemption, emit the trailing fence */
	kiq->pmf->kiq_unmap_queues(kiq_ring, ring, PREEMPT_QUEUES_NO_UNMAP,
				   ring->trail_fence_gpu_addr,
				   ++ring->trail_seq);

	ktime_get_ts64(&start);

	amdgpu_ring_commit(kiq_ring);

	spin_unlock_irqrestore(&kiq->ring_lock, flags);

	/* poll the trailing fence */
	for (i = 0; i < adev->usec_timeout; i++) {
		if (ring->trail_seq ==
		    le32_to_cpu(*(ring->trail_fence_cpu_addr)))
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout) {
		r = -EINVAL;
		DRM_ERROR("ring %d failed to preempt ib\n", ring->idx);
	}

	ktime_get_ts64(&end);
	duration = timespec64_sub(end, start);

	trace_gfx_v10_0_ring_preempt_ib(ring, &start, &end,
			div_u64(timespec64_to_ns(&duration), NSEC_PER_USEC));

	/* deassert preemption condition */
	amdgpu_ring_set_preempt_cond_exec(ring, true);
	return r;
}

static void gfx_v10_0_ring_emit_ce_meta(struct amdgpu_ring *ring, bool resume)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_ce_ib_state ce_payload = {0};
	uint64_t csa_addr;
	int cnt;

	cnt = (sizeof(ce_payload) >> 2) + 4 - 2;
	csa_addr = amdgpu_csa_vaddr(ring->adev);

	/**
	 * CSA Address shall be offset into the CSA allocated during driver
 	 * load based on HW Queue Index
 	 */
	csa_addr = csa_addr + AMDGPU_CSA_SIZE * ring->idx;

	amdgpu_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, cnt));
	amdgpu_ring_write(ring, (WRITE_DATA_ENGINE_SEL(2) |
				 WRITE_DATA_DST_SEL(8) |
				 WR_CONFIRM) |
				 WRITE_DATA_CACHE_POLICY(0));
	amdgpu_ring_write(ring, lower_32_bits(csa_addr +
			      offsetof(struct v10_gfx_meta_data, ce_payload)));
	amdgpu_ring_write(ring, upper_32_bits(csa_addr +
			      offsetof(struct v10_gfx_meta_data, ce_payload)));

	if (resume)
		amdgpu_ring_write_multiple(ring, adev->virt.csa_cpu_addr +
					   (AMDGPU_CSA_SIZE * ring->idx) +
					   offsetof(struct v10_gfx_meta_data,
						    ce_payload),
					   sizeof(ce_payload) >> 2);
	else
		amdgpu_ring_write_multiple(ring, (void *)&ce_payload,
					   sizeof(ce_payload) >> 2);
}

static void gfx_v10_0_ring_emit_de_meta(struct amdgpu_ring *ring, bool resume)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_de_ib_state de_payload = {0};
	uint64_t csa_addr;
	int cnt;

	csa_addr = amdgpu_csa_vaddr(ring->adev);

	/**
	 * CSA Address shall be offset into the CSA allocated during driver
 	 * load based on HW Queue Index
 	 */
	csa_addr = csa_addr + AMDGPU_CSA_SIZE * ring->idx;

	cnt = (sizeof(de_payload) >> 2) + 4 - 2;
	amdgpu_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, cnt));
	amdgpu_ring_write(ring, (WRITE_DATA_ENGINE_SEL(1) |
				 WRITE_DATA_DST_SEL(8) |
				 WR_CONFIRM) |
				 WRITE_DATA_CACHE_POLICY(0));
	amdgpu_ring_write(ring, lower_32_bits(csa_addr +
			      offsetof(struct v10_gfx_meta_data, de_payload)));
	amdgpu_ring_write(ring, upper_32_bits(csa_addr +
			      offsetof(struct v10_gfx_meta_data, de_payload)));

	if (resume)
		amdgpu_ring_write_multiple(ring, adev->virt.csa_cpu_addr +
					   (AMDGPU_CSA_SIZE * ring->idx) +
					   offsetof(struct v10_gfx_meta_data,
						    de_payload),
					   sizeof(de_payload) >> 2);
	else
		amdgpu_ring_write_multiple(ring, (void *)&de_payload,
					   sizeof(de_payload) >> 2);
}

static void gfx_v10_0_ring_emit_frame_cntl(struct amdgpu_ring *ring, bool start,
				    bool secure)
{
	uint32_t v = secure ? FRAME_TMZ : 0;

	amdgpu_ring_write(ring, PACKET3(PACKET3_FRAME_CONTROL, 0));
	amdgpu_ring_write(ring, v | FRAME_CMD(start ? 0 : 1));
}

static void gfx_v10_0_ring_emit_rreg(struct amdgpu_ring *ring, uint32_t reg,
				     uint32_t reg_val_offs)
{
	struct amdgpu_device *adev = ring->adev;

	amdgpu_ring_write(ring, PACKET3(PACKET3_COPY_DATA, 4));
	amdgpu_ring_write(ring, 0 |	/* src: register*/
				(5 << 8) |	/* dst: memory */
				(1 << 20));	/* write confirm */
	amdgpu_ring_write(ring, reg);
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, lower_32_bits(adev->wb.gpu_addr +
				reg_val_offs * 4));
	amdgpu_ring_write(ring, upper_32_bits(adev->wb.gpu_addr +
				reg_val_offs * 4));
}

static void gfx_v10_0_ring_emit_wreg(struct amdgpu_ring *ring, uint32_t reg,
				   uint32_t val)
{
	uint32_t cmd = 0;

	switch (ring->funcs->type) {
	case AMDGPU_RING_TYPE_GFX:
		cmd = WRITE_DATA_ENGINE_SEL(1) | WR_CONFIRM;
		break;
	case AMDGPU_RING_TYPE_KIQ:
		cmd = (1 << 16); /* no inc addr */
		break;
	default:
		cmd = WR_CONFIRM;
		break;
	}
	amdgpu_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
	amdgpu_ring_write(ring, cmd);
	amdgpu_ring_write(ring, reg);
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, val);
}

static void gfx_v10_0_ring_emit_reg_wait(struct amdgpu_ring *ring, uint32_t reg,
					uint32_t val, uint32_t mask)
{
	gfx_v10_0_wait_reg_mem(ring, 0, 0, 0, reg, 0, val, mask, 0x20);
}

static void gfx_v10_0_ring_emit_reg_write_reg_wait(struct amdgpu_ring *ring,
						   uint32_t reg0, uint32_t reg1,
						   uint32_t ref, uint32_t mask)
{
	int usepfp = (ring->funcs->type == AMDGPU_RING_TYPE_GFX);
	struct amdgpu_device *adev = ring->adev;
	bool fw_version_ok = false;

	fw_version_ok = adev->gfx.cp_fw_write_wait;

	/* M0&M1 FW can support read and write reg in one WAIT_REG_MEM cmd */
	if (fw_version_ok || adev->asic_type == CHIP_VANGOGH_LITE)
		gfx_v10_0_wait_reg_mem(ring, usepfp, 0, 1, reg0, reg1,
				       ref, mask, 0x20);
	else
		amdgpu_ring_emit_reg_write_reg_wait_helper(ring, reg0, reg1,
							   ref, mask);
}

#define REG_TYPE_INVALID		0
#define REG_TYPE_GLOBAL			1
#define REG_TYPE_GFX_PIPE_REG		2
#define REG_TYPE_GFX_PIPE_GRBM_REG	3

struct ft_reg_entry {
	uint32_t hwip;
	uint32_t inst;
	uint32_t seg;
	uint32_t reg_offset;
	uint32_t reg_mask;
	uint32_t reg_value;
};

static struct ft_reg_entry gfx_pipe_0_reg[] = {
		{ SOC15_REG_ENTRY(GC, 0, mmCP_RB0_RPTR),
				CP_RB0_RPTR__RB_RPTR_MASK, 0 },
		{ MAX_HWIP, 0, 0, 0, 0, 0 },
};

static struct ft_reg_entry gfx_pipe_grbm_reg[] = {
		{ SOC15_REG_ENTRY(GC, 0, mmCP_IB1_BUFSZ),
				0xFFFFFFFF, 0 },
		{ SOC15_REG_ENTRY(GC, 0, mmCP_IB1_BASE_HI),
				0xFFFFFFFF, 0 },
		{ SOC15_REG_ENTRY(GC, 0, mmCP_IB1_CMD_BUFSZ),
				0xFFFFFFFF, 0 },
		{ MAX_HWIP,	0, 0 },
};

struct fault_detect_reg_info {
	int type;
	int pipe;
	struct ft_reg_entry *reg_info_list;
};

static struct fault_detect_reg_info gfx_reg_info_list[] = {
		{ REG_TYPE_GFX_PIPE_REG,	PIPE_ID0,
				gfx_pipe_0_reg },
		{ REG_TYPE_GFX_PIPE_GRBM_REG,	PIPE_ID0,
				gfx_pipe_grbm_reg },
		{ REG_TYPE_INVALID,		0,
				0 },
};

#define FAULT_DETECT_DEBUG 0

int reg_info_list_read_and_compare(struct amdgpu_device *adev,
		struct ft_reg_entry *e, bool check_all)
{
	uint32_t tmp32, reg;
	int cnt = 0;

#if FAULT_DETECT_DEBUG
	const size_t max_buf_size = (64 * 1024);
	static char *buf;
	size_t size = 0, len = max_buf_size;

	if (buf == NULL) {
		buf = kmalloc(max_buf_size, GFP_KERNEL);
		if (buf == NULL) {
			dev_dbg(adev->dev, "can not create fault detect buf\n");
			return 0;
		}
	}
#endif


	while (e->hwip != MAX_HWIP) {

		reg = adev->reg_offset[e->hwip][e->inst][e->seg] +
				e->reg_offset;
		tmp32 = RREG32(reg);

#if FAULT_DETECT_DEBUG
		size += snprintf(buf + size, len - size,
			"  Fault POLL REG : %d : %d : %d : 0x%08x : 0x%08x\n",
			e->hwip, e->inst, e->seg, e->reg_offset, tmp32);
#endif
		trace_amdgpu_fault_detect_reg(e->hwip, e->inst, e->seg,
				e->reg_offset, tmp32);

		if (tmp32 != e->reg_value) {
			e->reg_value = tmp32;
			cnt++;	/* there is change */
			/* continue only if all register check required */
			if (!check_all)
				break;
		}
		e++;
	}

#if FAULT_DETECT_DEBUG
	buf[size++] = '\0';
	DRM_ERROR("%s", buf);
#endif

	return cnt;
}

static int gfx_fault_detect_check(struct amdgpu_device *adev, bool check_all)
{
	struct fault_detect_reg_info *pInfo;
	int cnt = 0, ret_cnt = 0;

	pInfo = &gfx_reg_info_list[0];
	while (pInfo->type != REG_TYPE_INVALID) {
		switch (pInfo->type) {
		case REG_TYPE_GFX_PIPE_REG:
			ret_cnt = reg_info_list_read_and_compare(adev,
					pInfo->reg_info_list, check_all);
			break;

		case REG_TYPE_GFX_PIPE_GRBM_REG:
			mutex_lock(&adev->srbm_mutex);
			nv_grbm_select(adev, 0, pInfo->pipe, 0, 0);
			ret_cnt = reg_info_list_read_and_compare(adev,
					pInfo->reg_info_list, check_all);
			nv_grbm_select(adev, 0, 0, 0, 0);
			mutex_unlock(&adev->srbm_mutex);
			break;
		}

		cnt += ret_cnt;
		/* do not continue if all register check is not required */
		if (cnt != 0 && !check_all)
			break;

		pInfo++;
	}
	/*
	 * cnt indicates number of registers we observed a change in value
	 * compared to previous sampled values. cnt zero indicates there
	 * were no change in the values and indicates a possible fault
	 */
	return (cnt);
}

/*
 * Indicates how many consecutive sample you allow to be same
 * before declaring a fault, if you set it to 2 it means if
 * there is no change in the registers value for 2 consecutive
 * poll we declare a fault
 */
#define GFX_FAULT_MAX_RETRY_CNT 2

struct fault_detect_state {
	bool isActive;
	int max_retry_cnt;
};

static struct fault_detect_state ft_gfx_state = {
	false,
	GFX_FAULT_MAX_RETRY_CNT
};

static bool gfx_v10_0_ring_hang_detect(void *handle, unsigned long flags)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	bool isGfxOn = false, fault = false;
	int diff_cnt;

	if (test_bit(FAULT_DETECT_FLAG_GFX, &flags))
		isGfxOn = true;

	/* check for GFX */
	if (!ft_gfx_state.isActive && isGfxOn) {

		/* take snapshot of all registers for the very first sample */
		gfx_fault_detect_check(adev, true);
		ft_gfx_state.max_retry_cnt = GFX_FAULT_MAX_RETRY_CNT;

	} else if (ft_gfx_state.isActive && isGfxOn) {

		/* take latest snapshot and compare for fault */
		diff_cnt = gfx_fault_detect_check(adev, false);

		/* there was not a single register value got changed */
		if (diff_cnt == 0)
			ft_gfx_state.max_retry_cnt--;
		else
			ft_gfx_state.max_retry_cnt = GFX_FAULT_MAX_RETRY_CNT;

		/* declare fault if we are not left with retry count */
		if (ft_gfx_state.max_retry_cnt == 0) {
			ft_gfx_state.max_retry_cnt = GFX_FAULT_MAX_RETRY_CNT;
			fault = true;
		}
	}

	ft_gfx_state.isActive = isGfxOn;
	return fault;
}

static void gfx_v10_0_ring_soft_recovery(struct amdgpu_ring *ring,
					 unsigned vmid)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t value = 0;

	value = REG_SET_FIELD(value, SQ_CMD, CMD, 0x03);
	value = REG_SET_FIELD(value, SQ_CMD, MODE, 0x01);
	value = REG_SET_FIELD(value, SQ_CMD, CHECK_VMID, 1);
	value = REG_SET_FIELD(value, SQ_CMD, VM_ID, vmid);
	WREG32_SOC15(GC, 0, mmSQ_CMD, value);
}

#if defined(CONFIG_DRM_AMDGPU_GFX_DUMP) || \
	defined(CONFIG_DRM_AMDGPU_COMPUTE_DUMP)
static size_t gfx_v10_0_ring_get_ring_commands(struct amdgpu_ring *ring,
					       char *buf, size_t len)
{
	unsigned int i;
	u64 temp64;
	u32 temp32;
	u32 num_dw = ring->funcs->align_mask + 1;
	char dump_string[80] = "";
	u32 last_seq = atomic_read(&ring->fence_drv.last_seq);
	u32 job_pending = ring->fence_drv.sync_seq - last_seq;
	size_t size = 0;

	if (job_pending == 0)
		return size;

	size = sgpu_dump_print(buf, len, "  Current execute ring frame:\n");
	temp64 = ring->wptr - (job_pending * (ring->funcs->align_mask + 1));
	for (i = 0; i < num_dw; i++) {
		temp32 = ring->ring[temp64 & ring->buf_mask];
		if (temp32 != ring->funcs->nop ||
		    i + 1 == num_dw ||
		    ring->ring[(temp64 + 1) & ring->buf_mask] !=
		    ring->funcs->nop) {
			size += sgpu_dump_print(buf + size, len - size,
					 "    [0x%016llx]=0x%08x\n",
					 temp64, temp32);
		} else {
			sgpu_dump_print(dump_string, sizeof(dump_string),
				 "[0x%016llx", temp64);
			for (; i < num_dw; i++) {
				if (ring->ring[temp64 & ring->buf_mask] !=
						ring->funcs->nop)
					break;
				temp64++;
			}
			size += sgpu_dump_print(buf + size, len - size,
					 "    %s...0x%016llx]=0x%08x\n",
					 dump_string, --temp64,
					 ring->funcs->nop);
		}
		temp64++;
	}

	return size;
}
#endif /* CONFIG_DRM_AMDGPU_GFX_DUMP || CONFIG_DRM_AMDGPU_COMPUTE_DUMP */

#if defined(CONFIG_DRM_AMDGPU_GFX_DUMP)
static size_t gfx_v10_0_ring_get_ring_status_gfx(struct amdgpu_ring *ring,
						 char *buf, size_t len)
{
	unsigned int i;
	bool is_idle, is_pfp_halted, is_me_halted;
	u32 temp32;
	u32 grbm_status, grbm_status2, cp_me_cntl;
	u64 temp64;
	const struct amd_ip_funcs *ip_funcs;
	struct amdgpu_device *adev = ring->adev;
	u32 last_seq = atomic_read(&ring->fence_drv.last_seq);
	u32 job_pending = ring->fence_drv.sync_seq - last_seq;
	bool is_gfxoff_on = false;
	size_t size = 0;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (adev->ip_blocks[i].version->type != AMD_IP_BLOCK_TYPE_GFX)
			continue;
		ip_funcs = adev->ip_blocks[i].version->funcs;
		if (ip_funcs->is_power_on) {
			is_gfxoff_on = ip_funcs->is_power_on((void *)adev);
			break;
		}
	}

	if (!is_gfxoff_on) {
		size += sgpu_dump_print(buf + size, len - size,
				 "Ring(%s): GfxOff_power_state(OFF)\n",
				 ring->name);
		return size;
	}

	cp_me_cntl = RREG32_SOC15(GC, 0, mmCP_ME_CNTL);
	is_pfp_halted = (cp_me_cntl & CP_ME_CNTL__PFP_HALT_MASK);
	is_me_halted = (cp_me_cntl & CP_ME_CNTL__ME_HALT_MASK);

	grbm_status = RREG32_SOC15(GC, 0, mmGRBM_STATUS);
	is_idle = !(grbm_status & GRBM_STATUS__GUI_ACTIVE_MASK);

	size += sgpu_dump_print(buf + size, len - size, "Graphics Pipe: %s\n",
			 is_idle ? "IDLE" : "BUSY");

	if (grbm_status & GRBM_STATUS__GUI_ACTIVE_MASK) {
		size += sgpu_dump_print(buf + size, len - size,
				 "GRBM_STATUS = 0x%08x\n", grbm_status);
		grbm_status2 = RREG32_SOC15(GC, 0, mmGRBM_STATUS2);
		size += sgpu_dump_print(buf + size, len - size,
				 "GRBM_STATUS2 = 0x%08x\n", grbm_status2);
		size += sgpu_dump_print(buf + size, len - size,
				 "  CPF_BUSY(%lu), CPG_BUSY(%lu), CPC_BUSY(%lu)\n",
				 ((grbm_status2 & GRBM_STATUS2__CPF_BUSY_MASK)
				  >> GRBM_STATUS2__CPF_BUSY__SHIFT),
				 ((grbm_status2 & GRBM_STATUS2__CPG_BUSY_MASK)
				  >> GRBM_STATUS2__CPG_BUSY__SHIFT),
				 ((grbm_status2 & GRBM_STATUS2__CPC_BUSY_MASK)
				  >> GRBM_STATUS2__CPC_BUSY__SHIFT));
		size += sgpu_dump_print(buf + size, len - size,
				 "  GE_BUSY(%lu), PA_BUSY(%lu)\n",
				 ((grbm_status & GRBM_STATUS__GE_BUSY_MASK)
				  >> GRBM_STATUS__GE_BUSY__SHIFT),
				 ((grbm_status & GRBM_STATUS__PA_BUSY_MASK)
				  >> GRBM_STATUS__PA_BUSY__SHIFT));
		size += sgpu_dump_print(buf + size, len - size,
				 "  SC_BUSY(%lu), SX_BUSY(%lu)\n",
				 ((grbm_status & GRBM_STATUS__SC_BUSY_MASK)
				  >> GRBM_STATUS__SC_BUSY__SHIFT),
				 ((grbm_status & GRBM_STATUS__SX_BUSY_MASK)
				  >> GRBM_STATUS__SX_BUSY__SHIFT));
		size += sgpu_dump_print(buf + size, len - size, "  SPI_BUSY(%lu)\n",
				 ((grbm_status & GRBM_STATUS__SPI_BUSY_MASK)
				  >> GRBM_STATUS__SPI_BUSY__SHIFT));
		size += sgpu_dump_print(buf + size, len - size,
				 "  TA_BUSY(%lu), TCP_BUSY(%lu)\n",
				 ((grbm_status & GRBM_STATUS__TA_BUSY_MASK)
				  >> GRBM_STATUS__TA_BUSY__SHIFT),
				 ((grbm_status2 & GRBM_STATUS2__TCP_BUSY_MASK)
				  >> GRBM_STATUS2__TCP_BUSY__SHIFT));
		size += sgpu_dump_print(buf + size, len - size,
				 "  CB_CLEAN(%lu), DB_CLEAN(%lu)\n",
				 ((grbm_status & GRBM_STATUS__CB_CLEAN_MASK)
				  >> GRBM_STATUS__CB_CLEAN__SHIFT),
				 ((grbm_status & GRBM_STATUS__DB_CLEAN_MASK)
				  >> GRBM_STATUS__DB_CLEAN__SHIFT));

		temp32 = RREG32_SOC15(GC, 0, mmGRBM_STATUS_SE0);
		size += sgpu_dump_print(buf + size, len - size,
				 "GRBM_STATUS_SE0 = 0x%08x\n", temp32);
		temp32 = RREG32_SOC15(GC, 0, mmGRBM_STATUS_SE1);
		size += sgpu_dump_print(buf + size, len - size,
				 "GRBM_STATUS_SE1 = 0x%08x\n", temp32);
		temp32 = RREG32_SOC15(GC, 0, mmGRBM_STATUS_SE2);
		size += sgpu_dump_print(buf + size, len - size,
				 "GRBM_STATUS_SE2 = 0x%08x\n", temp32);
		temp32 = RREG32_SOC15(GC, 0, mmGRBM_STATUS_SE3);
		size += sgpu_dump_print(buf + size, len - size,
				 "GRBM_STATUS_SE3 = 0x%08x\n", temp32);

		if (grbm_status2 &
			(GRBM_STATUS2__CPF_BUSY_MASK |
			 GRBM_STATUS2__CPG_BUSY_MASK |
			 GRBM_STATUS2__CPC_BUSY_MASK)) {
			temp32 = RREG32_SOC15(GC, 0, mmCP_ME_CNTL);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_ME_CNTL = 0x%08x\n", temp32);
			temp32 = RREG32_SOC15(GC, 0, mmCP_MEC_CNTL);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_MEC_CNTL = 0x%08x\n", temp32);
			temp32 = RREG32_SOC15(GC, 0, mmCP_STAT);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_STAT = 0x%08x\n", temp32);
			temp32 = RREG32_SOC15(GC, 0, mmCP_STALLED_STAT1);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_STALLED_STAT1 = 0x%08x\n",
					 temp32);
			temp32 = RREG32_SOC15(GC, 0, mmCP_STALLED_STAT2);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_STALLED_STAT2 = 0x%08x\n",
					 temp32);
			temp32 = RREG32_SOC15(GC, 0, mmCP_STALLED_STAT3);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_STALLED_STAT3 = 0x%08x\n",
					 temp32);
			temp32 = RREG32_SOC15(GC, 0, mmCP_CPF_STATUS);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_CPF_STATUS = 0x%08x\n", temp32);
			temp32 = RREG32_SOC15(GC, 0, mmCP_CPF_BUSY_STAT);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_CPF_BUSY_STAT = 0X%08x\n",
					 temp32);
			temp32 = RREG32_SOC15(GC, 0, mmCP_CPF_STALLED_STAT1);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_CPF_STALLED_STAT1 = 0x%08x\n",
					 temp32);
			temp32 = RREG32_SOC15(GC, 0, mmCP_CPC_STATUS);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_CPC_STATUS = 0x%08x\n", temp32);
			temp32 = RREG32_SOC15(GC, 0, mmCP_CPC_STALLED_STAT1);
			size += sgpu_dump_print(buf + size, len - size,
					 "CP_CPC_STALLED_STAT1 = 0x%08x\n",
					 temp32);
			size += sgpu_dump_print(buf + size, len - size,
						"CP_PFP_INSTR_PNTR= %#010x\n",
						RREG32_SOC15(GC, 0, mmCP_PFP_INSTR_PNTR));
			size += sgpu_dump_print(buf + size, len - size,
						"CP_ME_INSTR_PNTR= %#010x\n",
						RREG32_SOC15(GC, 0, mmCP_ME_INSTR_PNTR));
		}
	}

	size += sgpu_dump_print(buf + size, len - size,
			 "Ring(%s): pfp(%s), me(%s), status(%s) job_pending(%u)\n",
			 ring->name, (is_pfp_halted ? "HALTED" : "READY"),
			 (is_me_halted ? "HALTED" : "READY"),
			 (is_idle ? "IDLE" : "BUSY"), job_pending);

	if (!is_idle || job_pending) {
		if (ring->pipe == 1) {
			temp32 = RREG32_SOC15(GC, 0, mmCP_RB1_BASE_HI)
				& CP_RB1_BASE_HI__RB_BASE_HI_MASK;
			temp64 = (((u64)temp32 << 32) |
				RREG32_SOC15(GC, 0, mmCP_RB1_BASE)) << 8;
			size += sgpu_dump_print(buf + size, len - size,
					 "  GPU Addr (sw,hw):(0x%016llx, 0x%016llx)\n",
					 ring->gpu_addr, temp64);

			temp32 = RREG32_SOC15(GC, 0, mmCP_RB1_CNTL) &
				CP_RB1_CNTL__RB_BUFSZ_MASK;
			temp32 = 1 << (temp32 + 1);
			size += sgpu_dump_print(buf + size, len - size,
					 "  RB Size(sw,hw): (0x%08x,0x%08x)\n",
					 (ring->ring_size >> 2), temp32);

			temp32 = RREG32_SOC15(GC, 0, mmCP_RB1_RPTR) &
				CP_RB1_RPTR__RB_RPTR_MASK;
			size += sgpu_dump_print(buf + size, len - size,
					 "  RPTR: 0x%08x\n", temp32);

			temp32 = RREG32_SOC15(GC, 0, mmCP_RB1_WPTR_HI);
			temp64 = ((u64)temp32 << 32) |
				RREG32_SOC15(GC, 0, mmCP_RB1_WPTR);
			size += sgpu_dump_print(buf + size, len - size,
					 "  WPTR(sw,hw): (0x%016llx,0x%016llx)\n",
					 ring->wptr, temp64);
		} else {
			temp32 = RREG32_SOC15(GC, 0, mmCP_RB0_BASE_HI)
				& CP_RB0_BASE_HI__RB_BASE_HI_MASK;
			temp64 = (((u64)temp32 << 32) |
				RREG32_SOC15(GC, 0, mmCP_RB0_BASE)) << 8;
			size += sgpu_dump_print(buf + size, len - size,
					 "  GPU Addr (sw,hw):(0x%016llx, 0x%016llx)\n",
					 ring->gpu_addr, temp64);

			temp32 = RREG32_SOC15(GC, 0, mmCP_RB0_CNTL) &
				CP_RB0_CNTL__RB_BUFSZ_MASK;
			temp32 = 1 << (temp32 + 1);
			size += sgpu_dump_print(buf + size, len - size,
					 "  RB Size(sw,hw): (0x%08x,0x%08x)\n",
					 (ring->ring_size >> 2), temp32);

			temp32 = RREG32_SOC15(GC, 0, mmCP_RB0_RPTR) &
				CP_RB0_RPTR__RB_RPTR_MASK;
			size += sgpu_dump_print(buf + size, len - size,
					 "  RPTR: 0x%08x\n", temp32);

			temp32 = RREG32_SOC15(GC, 0, mmCP_RB0_WPTR_HI);
			temp64 = ((u64)temp32 << 32) |
				RREG32_SOC15(GC, 0, mmCP_RB0_WPTR);
			size += sgpu_dump_print(buf + size, len - size,
					 "  WPTR(sw,hw): (0x%016llx,0x%016llx)\n",
					 ring->wptr, temp64);
		}

		mutex_lock(&adev->srbm_mutex);
		gfx_v10_0_cp_gfx_switch_pipe(adev, (ring->pipe == 1) ?
					     PIPE_ID1 : PIPE_ID0);

		temp32 = (RREG32_SOC15(GC, 0, mmCP_IB1_BASE_HI) &
				CP_IB1_BASE_HI__IB1_BASE_HI_MASK);
		temp64 = ((uint64_t)temp32 << 32) |
				RREG32_SOC15(GC, 0, mmCP_IB1_BASE_LO);
		size += sgpu_dump_print(buf + size, len - size,
				 "  IB1 Base addr = 0x%016llx\n", temp64);

		temp32 = RREG32_SOC15(GC, 0, mmCP_IB1_CMD_BUFSZ) &
				CP_IB1_CMD_BUFSZ__IB1_CMD_REQSZ_MASK;
		size += sgpu_dump_print(buf + size, len - size,
				 "  IB1 buffer size = 0x%08x\n", temp32);

		temp32 = RREG32_SOC15(GC, 0, mmCP_IB2_BASE_HI) &
				CP_IB2_BASE_HI__IB2_BASE_HI_MASK;
		temp64 = ((uint64_t)temp32 << 32) |
				RREG32_SOC15(GC, 0, mmCP_IB2_BASE_LO);
		size += sgpu_dump_print(buf + size, len - size,
				 "  IB2 Base addr = 0x%016llx\n", temp64);

		size += sgpu_dump_print(buf + size, len - size,
				 "  PFP Last 8 decoded header:\n");
		for (i = 0; i < 8; i++) {
			temp32 = RREG32_SOC15(GC, 0, mmCP_PFP_HEADER_DUMP);
			size += sgpu_dump_print(buf + size, len - size,
					 "      0x%08x\n", temp32);
		}
		size += sgpu_dump_print(buf + size, len - size,
				 "    ME last 8 decoded header:\n");
		for (i = 0; i < 8; i++) {
			temp32 = RREG32_SOC15(GC, 0, mmCP_ME_HEADER_DUMP);
			size += sgpu_dump_print(buf + size, len - size,
					 "      0x%08x\n", temp32);
		}

		/* Switch to the default: pipe 0 */
		gfx_v10_0_cp_gfx_switch_pipe(adev, PIPE_ID0);
		mutex_unlock(&adev->srbm_mutex);

		size += gfx_v10_0_ring_get_ring_commands(ring, buf + size,
							 len - size);
	}

	return size;
}
#endif /* CONFIG_DRM_AMDGPU_GFX_DUMP */

#if defined(CONFIG_DRM_AMDGPU_COMPUTE_DUMP)
static size_t gfx_v10_0_ring_get_ring_status_compute(struct amdgpu_ring *ring,
						     char *buf, size_t len)
{
	unsigned int i;
	bool is_mec_halted, is_ring_mapped;
	u32 temp32, tmp;
	u64 temp64;
	const struct amd_ip_funcs *ip_funcs;
	struct amdgpu_device *adev = ring->adev;
	u32 last_seq = atomic_read(&ring->fence_drv.last_seq);
	u32 job_pending = ring->fence_drv.sync_seq - last_seq;
	bool is_gfxoff_on = false;
	size_t size = 0;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (adev->ip_blocks[i].version->type != AMD_IP_BLOCK_TYPE_GFX)
			continue;
		ip_funcs = adev->ip_blocks[i].version->funcs;
		if (ip_funcs->is_power_on) {
			is_gfxoff_on = ip_funcs->is_power_on((void *)adev);
			break;
		}
	}

	if (!is_gfxoff_on) {
		size = sgpu_dump_print(buf, len,
				"Ring(%s): GfxOff_power_state(OFF)\n",
				ring->name);
		return size;
	}

	temp32 = RREG32_SOC15(GC, 0, mmCP_MEC_CNTL);
	tmp = (ring->me == 1) ? CP_MEC_CNTL__MEC_ME1_HALT_MASK :
		CP_MEC_CNTL__MEC_ME2_HALT_MASK;
	is_mec_halted = (temp32 & tmp);

	mutex_lock(&adev->srbm_mutex);
	nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);

	temp32 = RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE);
	is_ring_mapped = (temp32 & CP_HQD_ACTIVE__ACTIVE_MASK);

	size += sgpu_dump_print(buf + size, len - size,
			 "Ring(%s): mec(%s), ring(%s), job_pending(%u)\n",
			 ring->name, (is_mec_halted ? "HALTED" : "READY"),
			 (is_ring_mapped ? "ENABLED" : "DISABLED"),
			 job_pending);

	if (job_pending) {
		temp32 = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_BASE_HI) &
				CP_HQD_PQ_BASE_HI__ADDR_HI_MASK;
		temp64 = (((uint64_t)temp32 << 32) |
				RREG32_SOC15(GC, 0, mmCP_HQD_PQ_BASE)) << 8;
		size += sgpu_dump_print(buf + size, len - size,
				 "  GPUAddress (sw,hw): (0x%016llx, 0x%016llx)\n",
				 ring->gpu_addr, temp64);

		temp32 = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_CONTROL) &
				CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK;
		temp32 = (1 << (temp32 + 1));
		size += sgpu_dump_print(buf + size, len - size,
				 "  Size(sw,hw): (0x%08x,0x%08x)\n",
				 (ring->ring_size >> 2), temp32);

		temp32 = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_HI);
		temp64 = (((uint64_t)temp32 << 32) |
				RREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_LO));
		size += sgpu_dump_print(buf + size, len - size,
				 "  WPTR(sw,hw): (0x%016llx,0x%016llx)\n",
				 ring->wptr, temp64);

		temp32 = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR);
		size += sgpu_dump_print(buf + size, len - size,
				 "  RPTR = 0x%08x\n", temp32);

		temp32 = RREG32_SOC15(GC, 0, mmCP_HQD_IB_BASE_ADDR_HI) &
				CP_HQD_IB_BASE_ADDR_HI__IB_BASE_ADDR_HI_MASK;
		temp64 = (((uint64_t)temp32 << 32) |
				RREG32_SOC15(GC, 0, mmCP_HQD_IB_BASE_ADDR));
		size += sgpu_dump_print(buf + size, len - size,
				 "  IB Base Address = 0x%016llx\n", temp64);

		temp32 = RREG32_SOC15(GC, 0, mmCP_HQD_IB_CONTROL);
		size += sgpu_dump_print(buf + size, len - size,
				 "  CP_HQD_IB_CONTROL = 0x%08x\n", temp32);
		tmp = temp32 & CP_HQD_IB_CONTROL__IB_SIZE_MASK;
		size += sgpu_dump_print(buf + size, len - size,
				 "    IB_SIZE = 0x%08x\n", tmp);
		tmp = (temp32 & CP_HQD_IB_CONTROL__PROCESSING_IB_MASK) >>
				CP_HQD_IB_CONTROL__PROCESSING_IB__SHIFT;
		size += sgpu_dump_print(buf + size, len - size,
				 "    PROCESSING_IB = %d\n", tmp);

		temp32 = RREG32_SOC15(GC, 0, mmCP_HQD_IB_RPTR) &
				CP_HQD_IB_RPTR__CONSUMED_OFFSET_MASK;
		size += sgpu_dump_print(buf + size, len - size,
				 "  CP_HQD_IB_RPTR = 0x%08x\n", temp32);

		size += sgpu_dump_print(buf + size, len - size,
				 "  MEC last 8 decoded header:\n");
		if (ring->me == 1)
			tmp = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME1_HEADER_DUMP);
		else
			tmp = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME2_HEADER_DUMP);
		for (i = 0; i < 8; i++) {
			temp32 = RREG32(tmp);
			size += sgpu_dump_print(buf + size, len - size,
					 "    0x%08x\n", temp32);
		}

		size += gfx_v10_0_ring_get_ring_commands(ring,
							 buf + size,
							 len - size);
	}
	nv_grbm_select(adev, 0, 0, 0, 0);
	mutex_unlock(&adev->srbm_mutex);

	return size;
}
#endif /* CONFIG_DRM_AMDGPU_COMPUTE_DUMP */

static void
gfx_v10_0_set_gfx_eop_interrupt_state(struct amdgpu_device *adev,
				      uint32_t me, uint32_t pipe,
				      enum amdgpu_interrupt_state state)
{
	uint32_t cp_int_cntl, cp_int_cntl_reg;

	if (!me) {
		switch (pipe) {
		case 0:
			cp_int_cntl_reg = SOC15_REG_OFFSET(GC, 0, mmCP_INT_CNTL_RING0);
			break;
		case 1:
			cp_int_cntl_reg = SOC15_REG_OFFSET(GC, 0, mmCP_INT_CNTL_RING1);
			break;
		default:
			DRM_DEBUG("invalid pipe %d\n", pipe);
			return;
		}
	} else {
		DRM_DEBUG("invalid me %d\n", me);
		return;
	}

	switch (state) {
	case AMDGPU_IRQ_STATE_DISABLE:
		cp_int_cntl = RREG32(cp_int_cntl_reg);
		cp_int_cntl = REG_SET_FIELD(cp_int_cntl, CP_INT_CNTL_RING0,
					    TIME_STAMP_INT_ENABLE, 0);
		WREG32(cp_int_cntl_reg, cp_int_cntl);
		break;
	case AMDGPU_IRQ_STATE_ENABLE:
		cp_int_cntl = RREG32(cp_int_cntl_reg);
		cp_int_cntl = REG_SET_FIELD(cp_int_cntl, CP_INT_CNTL_RING0,
					    TIME_STAMP_INT_ENABLE, 1);
		WREG32(cp_int_cntl_reg, cp_int_cntl);
		break;
	default:
		break;
	}
}

static void gfx_v10_0_set_compute_eop_interrupt_state(struct amdgpu_device *adev,
						     int me, int pipe,
						     enum amdgpu_interrupt_state state)
{
	u32 mec_int_cntl, mec_int_cntl_reg;

	/*
	 * amdgpu controls only the first MEC. That's why this function only
	 * handles the setting of interrupts for this specific MEC. All other
	 * pipes' interrupts are set by amdkfd.
	 */

	if (me == 1) {
		switch (pipe) {
		case 0:
			mec_int_cntl_reg = SOC15_REG_OFFSET(GC, 0, mmCP_ME1_PIPE0_INT_CNTL);
			break;
		case 1:
			mec_int_cntl_reg = SOC15_REG_OFFSET(GC, 0, mmCP_ME1_PIPE1_INT_CNTL);
			break;
		case 2:
			mec_int_cntl_reg = SOC15_REG_OFFSET(GC, 0, mmCP_ME1_PIPE2_INT_CNTL);
			break;
		case 3:
			mec_int_cntl_reg = SOC15_REG_OFFSET(GC, 0, mmCP_ME1_PIPE3_INT_CNTL);
			break;
		default:
			DRM_DEBUG("invalid pipe %d\n", pipe);
			return;
		}
	} else {
		DRM_DEBUG("invalid me %d\n", me);
		return;
	}

	switch (state) {
	case AMDGPU_IRQ_STATE_DISABLE:
		mec_int_cntl = RREG32(mec_int_cntl_reg);
		mec_int_cntl = REG_SET_FIELD(mec_int_cntl, CP_ME1_PIPE0_INT_CNTL,
					     TIME_STAMP_INT_ENABLE, 0);
		WREG32(mec_int_cntl_reg, mec_int_cntl);
		break;
	case AMDGPU_IRQ_STATE_ENABLE:
		mec_int_cntl = RREG32(mec_int_cntl_reg);
		mec_int_cntl = REG_SET_FIELD(mec_int_cntl, CP_ME1_PIPE0_INT_CNTL,
					     TIME_STAMP_INT_ENABLE, 1);
		WREG32(mec_int_cntl_reg, mec_int_cntl);
		break;
	default:
		break;
	}
}

static int gfx_v10_0_set_eop_interrupt_state(struct amdgpu_device *adev,
					    struct amdgpu_irq_src *src,
					    unsigned type,
					    enum amdgpu_interrupt_state state)
{
	switch (type) {
	case AMDGPU_CP_IRQ_GFX_ME0_PIPE0_EOP:
		gfx_v10_0_set_gfx_eop_interrupt_state(adev, 0, 0, state);
		break;
	case AMDGPU_CP_IRQ_GFX_ME0_PIPE1_EOP:
		gfx_v10_0_set_gfx_eop_interrupt_state(adev, 0, 1, state);
		break;
	case AMDGPU_CP_IRQ_COMPUTE_MEC1_PIPE0_EOP:
		gfx_v10_0_set_compute_eop_interrupt_state(adev, 1, 0, state);
		break;
	case AMDGPU_CP_IRQ_COMPUTE_MEC1_PIPE1_EOP:
		gfx_v10_0_set_compute_eop_interrupt_state(adev, 1, 1, state);
		break;
	case AMDGPU_CP_IRQ_COMPUTE_MEC1_PIPE2_EOP:
		gfx_v10_0_set_compute_eop_interrupt_state(adev, 1, 2, state);
		break;
	case AMDGPU_CP_IRQ_COMPUTE_MEC1_PIPE3_EOP:
		gfx_v10_0_set_compute_eop_interrupt_state(adev, 1, 3, state);
		break;
	case AMDGPU_CP_IRQ_COMPUTE_MEC2_PIPE0_EOP:
		gfx_v10_0_set_compute_eop_interrupt_state(adev, 2, 0, state);
		break;
	case AMDGPU_CP_IRQ_COMPUTE_MEC2_PIPE1_EOP:
		gfx_v10_0_set_compute_eop_interrupt_state(adev, 2, 1, state);
		break;
	case AMDGPU_CP_IRQ_COMPUTE_MEC2_PIPE2_EOP:
		gfx_v10_0_set_compute_eop_interrupt_state(adev, 2, 2, state);
		break;
	case AMDGPU_CP_IRQ_COMPUTE_MEC2_PIPE3_EOP:
		gfx_v10_0_set_compute_eop_interrupt_state(adev, 2, 3, state);
		break;
	default:
		break;
	}
	return 0;
}

static int gfx_v10_0_eop_irq(struct amdgpu_device *adev,
			     struct amdgpu_irq_src *source,
			     struct amdgpu_iv_entry *entry)
{
	int i;
	u8 me_id, pipe_id, queue_id;
	struct amdgpu_ring *ring;
	struct amdgpu_sws *sws;

	DRM_DEBUG("IH: CP EOP\n");
	me_id = (entry->ring_id & 0x0c) >> 2;
	pipe_id = (entry->ring_id & 0x03) >> 0;
	queue_id = (entry->ring_id & 0x70) >> 4;

	SGPU_LOG(adev, DMSG_INFO, DMSG_ETC, "me=%d, pipe=%d, queue=%d",
		          me_id, pipe_id, queue_id);

	switch (me_id) {
	case 0:
		if (adev->asic_type != CHIP_VANGOGH_LITE)
			queue_id = entry->src_data[0] & 0x0f;

		amdgpu_fence_process(&adev->gfx.gfx_ring[queue_id]);

		break;
	case 1:
	case 2:
		for (i = 0; i < adev->gfx.num_compute_rings; i++) {
			ring = &adev->gfx.compute_ring[i];
			/* Per-queue interrupt is supported for MEC starting from VI.
			 * The interrupt can only be enabled/disabled per pipe instead of per queue.
			 */
			if (ring->me == me_id && ring->pipe == pipe_id &&
			    ring->queue == queue_id) {
				amdgpu_fence_process(ring);
				return 0;
			}
		}

		sws = &adev->sws;
		if (me_id == (AMDGPU_TMZ_MEC + 1) &&
		    pipe_id == AMDGPU_TMZ_PIPE &&
		    queue_id == AMDGPU_TMZ_QUEUE &&
		    amdgpu_tmz) {
			queue_work(sws->sched, &sws->eop_work);
			return 0;
		}

		if (cwsr_enable && sws->ctx_num) {
			queue_work(sws->sched, &sws->eop_work);
		}
		break;
	}

	return 0;
}

static int gfx_v10_0_set_priv_reg_fault_state(struct amdgpu_device *adev,
					      struct amdgpu_irq_src *source,
					      unsigned type,
					      enum amdgpu_interrupt_state state)
{
	switch (state) {
	case AMDGPU_IRQ_STATE_DISABLE:
	case AMDGPU_IRQ_STATE_ENABLE:
		WREG32_FIELD15(GC, 0, CP_INT_CNTL_RING0,
			       PRIV_REG_INT_ENABLE,
			       state == AMDGPU_IRQ_STATE_ENABLE ? 1 : 0);
		break;
	default:
		break;
	}

	return 0;
}

static int gfx_v10_0_set_priv_inst_fault_state(struct amdgpu_device *adev,
					       struct amdgpu_irq_src *source,
					       unsigned type,
					       enum amdgpu_interrupt_state state)
{
	switch (state) {
	case AMDGPU_IRQ_STATE_DISABLE:
		fallthrough;
	case AMDGPU_IRQ_STATE_ENABLE:
		WREG32_FIELD15(GC, 0, CP_INT_CNTL_RING0,
			       PRIV_INSTR_INT_ENABLE,
			       state == AMDGPU_IRQ_STATE_ENABLE ? 1 : 0);
		break;
	default:
		break;
	}

	return 0;
}

static void gfx_v10_0_handle_priv_fault(struct amdgpu_device *adev,
					struct amdgpu_iv_entry *entry)
{
	int i;
	u8 me_id, pipe_id, queue_id;
	struct amdgpu_ring *ring;
	struct amdgpu_sws *sws;

	me_id = (entry->ring_id & 0x0c) >> 2;
	pipe_id = (entry->ring_id & 0x03) >> 0;
	queue_id = (entry->ring_id & 0x70) >> 4;

	switch (me_id) {
	case 0:
		for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
			ring = &adev->gfx.gfx_ring[i];
			/* we only enabled 1 gfx queue per pipe for now */
			if (ring->me == me_id && ring->pipe == pipe_id)
				drm_sched_fault(&ring->sched);
		}
		break;
	case 1:
	case 2:
		for (i = 0; i < adev->gfx.num_compute_rings; i++) {
			ring = &adev->gfx.compute_ring[i];
			if (ring->me == me_id && ring->pipe == pipe_id &&
			    ring->queue == queue_id) {
				drm_sched_fault(&ring->sched);
				return;
			}
		}

		sws = &adev->sws;
		if (me_id == (AMDGPU_TMZ_MEC + 1) &&
		    pipe_id == AMDGPU_TMZ_PIPE &&
		    queue_id == AMDGPU_TMZ_QUEUE &&
		    amdgpu_tmz) {
			queue_work(sws->sched, &sws->tmz_fault_work);
			return;
		}

		if (cwsr_enable)
			queue_work(sws->sched, &sws->cwsr_fault_work);
		break;
	default:
		BUG();
	}
}

static int gfx_v10_0_priv_reg_irq(struct amdgpu_device *adev,
				  struct amdgpu_irq_src *source,
				  struct amdgpu_iv_entry *entry)
{
	DRM_ERROR("Illegal register access in command stream\n");
	gfx_v10_0_handle_priv_fault(adev, entry);
	return 0;
}

static int gfx_v10_0_priv_inst_irq(struct amdgpu_device *adev,
				   struct amdgpu_irq_src *source,
				   struct amdgpu_iv_entry *entry)
{
	DRM_ERROR("Illegal instruction in command stream\n");
	gfx_v10_0_handle_priv_fault(adev, entry);
	return 0;
}

static int gfx_v10_0_kiq_set_interrupt_state(struct amdgpu_device *adev,
					     struct amdgpu_irq_src *src,
					     unsigned int type,
					     enum amdgpu_interrupt_state state)
{
	uint32_t tmp, target;
	struct amdgpu_ring *ring = &(adev->gfx.kiq.ring);

	if (ring->me == 1)
		target = SOC15_REG_OFFSET(GC, 0, mmCP_ME1_PIPE0_INT_CNTL);
	else
		target = SOC15_REG_OFFSET(GC, 0, mmCP_ME2_PIPE0_INT_CNTL);
	target += ring->pipe;

	switch (type) {
	case AMDGPU_CP_KIQ_IRQ_DRIVER0:
		if (state == AMDGPU_IRQ_STATE_DISABLE) {
			tmp = RREG32_SOC15(GC, 0, mmCPC_INT_CNTL);
			tmp = REG_SET_FIELD(tmp, CPC_INT_CNTL,
					    GENERIC2_INT_ENABLE, 0);
			WREG32_SOC15(GC, 0, mmCPC_INT_CNTL, tmp);

			tmp = RREG32(target);
			tmp = REG_SET_FIELD(tmp, CP_ME2_PIPE0_INT_CNTL,
					    GENERIC2_INT_ENABLE, 0);
			WREG32(target, tmp);
		} else {
			tmp = RREG32_SOC15(GC, 0, mmCPC_INT_CNTL);
			tmp = REG_SET_FIELD(tmp, CPC_INT_CNTL,
					    GENERIC2_INT_ENABLE, 1);
			WREG32_SOC15(GC, 0, mmCPC_INT_CNTL, tmp);

			tmp = RREG32(target);
			tmp = REG_SET_FIELD(tmp, CP_ME2_PIPE0_INT_CNTL,
					    GENERIC2_INT_ENABLE, 1);
			WREG32(target, tmp);
		}
		break;
	default:
		BUG(); /* kiq only support GENERIC2_INT now */
		break;
	}
	return 0;
}

static int gfx_v10_0_kiq_irq(struct amdgpu_device *adev,
			     struct amdgpu_irq_src *source,
			     struct amdgpu_iv_entry *entry)
{
	u8 me_id, pipe_id, queue_id;
	struct amdgpu_ring *ring = &(adev->gfx.kiq.ring);

	me_id = (entry->ring_id & 0x0c) >> 2;
	pipe_id = (entry->ring_id & 0x03) >> 0;
	queue_id = (entry->ring_id & 0x70) >> 4;
	DRM_DEBUG("IH: CPC GENERIC2_INT, me:%d, pipe:%d, queue:%d\n",
		   me_id, pipe_id, queue_id);

	amdgpu_fence_process(ring);
	return 0;
}

static void gfx_v10_0_emit_mem_sync(struct amdgpu_ring *ring)
{
	const unsigned int gcr_cntl =
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_INV(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_WB(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_INV(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_WB(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GL1_INV(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GLV_INV(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GLK_INV(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GLI_INV(1);

	/* ACQUIRE_MEM - make one or more surfaces valid for use by the subsequent operations */
	amdgpu_ring_write(ring, PACKET3(PACKET3_ACQUIRE_MEM, 6));
	amdgpu_ring_write(ring, 0); /* CP_COHER_CNTL */
	amdgpu_ring_write(ring, 0xffffffff);  /* CP_COHER_SIZE */
	amdgpu_ring_write(ring, 0xffffff);  /* CP_COHER_SIZE_HI */
	amdgpu_ring_write(ring, 0); /* CP_COHER_BASE */
	amdgpu_ring_write(ring, 0);  /* CP_COHER_BASE_HI */
	amdgpu_ring_write(ring, 0x0000000A); /* POLL_INTERVAL */
	amdgpu_ring_write(ring, gcr_cntl); /* GCR_CNTL */
}

static const struct amd_ip_funcs gfx_v10_0_ip_funcs = {
	.name = "gfx_v10_0",
	.early_init = gfx_v10_0_early_init,
	.late_init = gfx_v10_0_late_init,
	.fw_init = gfx_v10_0_fw_init,
	.sw_init = gfx_v10_0_sw_init,
	.sw_fini = gfx_v10_0_sw_fini,
	.hw_init = gfx_v10_0_hw_init,
	.hw_fini = gfx_v10_0_hw_fini,
	.suspend = gfx_v10_0_suspend,
	.resume = gfx_v10_0_resume,
	.is_idle = gfx_v10_0_is_idle,
	.wait_for_idle = gfx_v10_0_wait_for_idle,
	.check_soft_reset = gfx_v10_0_check_soft_reset,
	.pre_soft_reset = gfx_v10_0_pre_soft_reset,
	.soft_reset = gfx_v10_0_soft_reset,
	.post_soft_reset = gfx_v10_0_post_soft_reset,
	.set_clockgating_state = gfx_v10_0_set_clockgating_state,
	.set_powergating_state = gfx_v10_0_set_powergating_state,
	.get_clockgating_state = gfx_v10_0_get_clockgating_state,
#if defined(CONFIG_DRM_AMDGPU_DUMP)
	.is_power_on = gfx_v10_0_is_power_on,
#endif
	.fault_detect = gfx_v10_0_ring_hang_detect,
};

static const struct amdgpu_ring_funcs gfx_v10_0_ring_funcs_gfx = {
	.type = AMDGPU_RING_TYPE_GFX,
	.align_mask = 0xff,
	.nop = PACKET3(PACKET3_NOP, 0x3FFF),
	.support_64bit_ptrs = true,
	.vmhub = AMDGPU_GFXHUB_0,
	.get_rptr = gfx_v10_0_ring_get_rptr_gfx,
	.get_rreg = gfx_v10_0_ring_get_rreg_gfx,
	.get_wptr = gfx_v10_0_ring_get_wptr_gfx,
	.set_wptr = gfx_v10_0_ring_set_wptr_gfx,
	.emit_frame_size = /* totally 242 maximum if 16 IBs */
		5 + /* COND_EXEC */
		7 + /* PIPELINE_SYNC */
		SOC15_FLUSH_GPU_TLB_NUM_WREG * 5 +
		SOC15_FLUSH_GPU_TLB_NUM_REG_WAIT * 7 +
		2 + /* VM_FLUSH */
		8 + /* FENCE for VM_FLUSH */
		20 + /* GDS switch */
		4 + /* double SWITCH_BUFFER,
		     * the first COND_EXEC jump to the place
		     * just prior to this double SWITCH_BUFFER
		     */
		5 + /* COND_EXEC */
		7 + /* HDP_flush */
		4 + /* VGT_flush */
		14 + /*	CE_META */
		31 + /*	DE_META */
		3 + /* CNTX_CTRL */
		5 + /* HDP_INVL */
		8 + 8 + /* FENCE x2 */
		2 + /* SWITCH_BUFFER */
		8, /* gfx_v10_0_emit_mem_sync */
	.emit_ib_size =	4, /* gfx_v10_0_ring_emit_ib_gfx */
	.emit_ib = gfx_v10_0_ring_emit_ib_gfx,
	.emit_fence = gfx_v10_0_ring_emit_fence,
	.emit_pipeline_sync = gfx_v10_0_ring_emit_pipeline_sync,
	.emit_vm_flush = gfx_v10_0_ring_emit_vm_flush,
	.emit_gds_switch = gfx_v10_0_ring_emit_gds_switch,
	.emit_hdp_flush = gfx_v10_0_ring_emit_hdp_flush,
	.test_ring = gfx_v10_0_ring_test_ring,
	.test_ib = gfx_v10_0_ring_test_ib,
	.insert_nop = amdgpu_ring_insert_nop,
	.pad_ib = amdgpu_ring_generic_pad_ib,
	.emit_switch_buffer = gfx_v10_0_ring_emit_sb,
	.emit_cntxcntl = gfx_v10_0_ring_emit_cntxcntl,
	.init_cond_exec = gfx_v10_0_ring_emit_init_cond_exec,
	.patch_cond_exec = gfx_v10_0_ring_emit_patch_cond_exec,
	.preempt_ib = gfx_v10_0_ring_preempt_ib,
	.emit_frame_cntl = gfx_v10_0_ring_emit_frame_cntl,
	.emit_wreg = gfx_v10_0_ring_emit_wreg,
	.emit_reg_wait = gfx_v10_0_ring_emit_reg_wait,
	.emit_reg_write_reg_wait = gfx_v10_0_ring_emit_reg_write_reg_wait,
	.soft_recovery = gfx_v10_0_ring_soft_recovery,
	.emit_mem_sync = gfx_v10_0_emit_mem_sync,
	.check_ring_done =  gfx_v10_0_check_done,
#if defined(CONFIG_DRM_AMDGPU_GFX_DUMP)
	.get_ring_status = gfx_v10_0_ring_get_ring_status_gfx,
#endif
};

static const struct amdgpu_ring_funcs gfx_v10_0_ring_funcs_compute = {
	.type = AMDGPU_RING_TYPE_COMPUTE,
	.align_mask = 0xff,
	.nop = PACKET3(PACKET3_NOP, 0x3FFF),
	.support_64bit_ptrs = true,
	.vmhub = AMDGPU_GFXHUB_0,
	.get_rptr = gfx_v10_0_ring_get_rptr_compute,
	.get_rreg = gfx_v10_0_ring_get_rreg_compute,
	.get_wptr = gfx_v10_0_ring_get_wptr_compute,
	.set_wptr = gfx_v10_0_ring_set_wptr_compute,
	.emit_frame_size =
		20 + /* gfx_v10_0_ring_emit_gds_switch */
		7 + /* gfx_v10_0_ring_emit_hdp_flush */
		5 + /* hdp invalidate */
		7 + /* gfx_v10_0_ring_emit_pipeline_sync */
		SOC15_FLUSH_GPU_TLB_NUM_WREG * 5 +
		SOC15_FLUSH_GPU_TLB_NUM_REG_WAIT * 7 +
		2 + /* gfx_v10_0_ring_emit_vm_flush */
		8 + 8 + 8 + /* gfx_v10_0_ring_emit_fence x3 for user fence, vm fence */
		8, /* gfx_v10_0_emit_mem_sync */
	.emit_ib_size =	7, /* gfx_v10_0_ring_emit_ib_compute */
	.emit_ib = gfx_v10_0_ring_emit_ib_compute,
	.emit_fence = gfx_v10_0_ring_emit_fence,
	.emit_pipeline_sync = gfx_v10_0_ring_emit_pipeline_sync,
	.emit_vm_flush = gfx_v10_0_ring_emit_vm_flush,
	.emit_gds_switch = gfx_v10_0_ring_emit_gds_switch,
	.emit_hdp_flush = gfx_v10_0_ring_emit_hdp_flush,
	.test_ring = gfx_v10_0_ring_test_ring,
	.test_ib = gfx_v10_0_ring_test_ib,
	.insert_nop = amdgpu_ring_insert_nop,
	.pad_ib = amdgpu_ring_generic_pad_ib,
	.emit_wreg = gfx_v10_0_ring_emit_wreg,
	.emit_reg_wait = gfx_v10_0_ring_emit_reg_wait,
	.emit_reg_write_reg_wait = gfx_v10_0_ring_emit_reg_write_reg_wait,
	.emit_mem_sync = gfx_v10_0_emit_mem_sync,
	.check_ring_done =  gfx_v10_0_check_done,
#if defined(CONFIG_DRM_AMDGPU_COMPUTE_DUMP)
	.get_ring_status = gfx_v10_0_ring_get_ring_status_compute,
#endif
	.compute_mqd_init = gfx_v10_0_compute_cwsr_mqd_init,
	.compute_mqd_update = gfx_v10_0_compute_mqd_update,
};

static const struct amdgpu_ring_funcs gfx_v10_0_ring_funcs_kiq = {
	.type = AMDGPU_RING_TYPE_KIQ,
	.align_mask = 0xff,
	.nop = PACKET3(PACKET3_NOP, 0x3FFF),
	.support_64bit_ptrs = true,
	.vmhub = AMDGPU_GFXHUB_0,
	.get_rptr = gfx_v10_0_ring_get_rptr_compute,
	.get_wptr = gfx_v10_0_ring_get_wptr_compute,
	.set_wptr = gfx_v10_0_ring_set_wptr_compute,
	.emit_frame_size =
		20 + /* gfx_v10_0_ring_emit_gds_switch */
		7 + /* gfx_v10_0_ring_emit_hdp_flush */
		5 + /*hdp invalidate */
		7 + /* gfx_v10_0_ring_emit_pipeline_sync */
		SOC15_FLUSH_GPU_TLB_NUM_WREG * 5 +
		SOC15_FLUSH_GPU_TLB_NUM_REG_WAIT * 7 +
		2 + /* gfx_v10_0_ring_emit_vm_flush */
		8 + 8 + 8, /* gfx_v10_0_ring_emit_fence_kiq x3 for user fence, vm fence */
	.emit_ib_size =	7, /* gfx_v10_0_ring_emit_ib_compute */
	.emit_ib = gfx_v10_0_ring_emit_ib_compute,
	.emit_fence = gfx_v10_0_ring_emit_fence_kiq,
	.test_ring = gfx_v10_0_ring_test_ring,
	.test_ib = gfx_v10_0_ring_test_ib,
	.insert_nop = amdgpu_ring_insert_nop,
	.pad_ib = amdgpu_ring_generic_pad_ib,
	.emit_rreg = gfx_v10_0_ring_emit_rreg,
	.emit_wreg = gfx_v10_0_ring_emit_wreg,
	.emit_reg_wait = gfx_v10_0_ring_emit_reg_wait,
	.emit_reg_write_reg_wait = gfx_v10_0_ring_emit_reg_write_reg_wait,
#if defined(CONFIG_DRM_AMDGPU_COMPUTE_DUMP)
	.get_ring_status = gfx_v10_0_ring_get_ring_status_compute,
#endif
};

static void gfx_v10_0_set_ring_funcs(struct amdgpu_device *adev)
{
	int i;

	adev->gfx.kiq.ring.funcs = &gfx_v10_0_ring_funcs_kiq;

	for (i = 0; i < adev->gfx.num_gfx_rings; i++)
		adev->gfx.gfx_ring[i].funcs = &gfx_v10_0_ring_funcs_gfx;

	for (i = 0; i < adev->gfx.num_compute_rings; i++)
		adev->gfx.compute_ring[i].funcs = &gfx_v10_0_ring_funcs_compute;
}

static const struct amdgpu_irq_src_funcs gfx_v10_0_eop_irq_funcs = {
	.set = gfx_v10_0_set_eop_interrupt_state,
	.process = gfx_v10_0_eop_irq,
};

static const struct amdgpu_irq_src_funcs gfx_v10_0_priv_reg_irq_funcs = {
	.set = gfx_v10_0_set_priv_reg_fault_state,
	.process = gfx_v10_0_priv_reg_irq,
};

static const struct amdgpu_irq_src_funcs gfx_v10_0_priv_inst_irq_funcs = {
	.set = gfx_v10_0_set_priv_inst_fault_state,
	.process = gfx_v10_0_priv_inst_irq,
};

static const struct amdgpu_irq_src_funcs gfx_v10_0_kiq_irq_funcs = {
	.set = gfx_v10_0_kiq_set_interrupt_state,
	.process = gfx_v10_0_kiq_irq,
};

static const struct sgpu_bpmd_funcs gfx_v10_0_bpmd_funcs = {
	.dump_header = sgpu_bpmd_layout_dump_header,
	.dump_reg = gfx_v10_0_bpmd_dump_reg,
	.dump_ring = gfx_v10_0_bpmd_dump_ring,
	.dump_bo = sgpu_bpmd_layout_dump_bo_packet,
	.dump_system_info = sgpu_bpmd_layout_dump_system_info_packet,
	.dump_footer = sgpu_bpmd_layout_dump_footer_packet,

	.find_ibs = gfx_v10_0_bpmd_find_ibs,
};

static const struct dpm_hw_utilization_funcs gfx_v10_0_dpm_hw_util_funcs = {
	.init = vangogh_lite_hw_utilization_init,
	.reset = vangogh_lite_reset_hw_utilization,
	.get_hw_time = vangogh_lite_get_hw_time,
};

static int gfx_v10_0_bpmd_dump_reg_ta(struct sgpu_bpmd_output *sbo,
				      struct amdgpu_device *adev)
{
	int r = 0;
	size_t i = 0;
	const u32 domain = SGPU_BPMD_LAYOUT_REG_DOMAIN_SFR;
	uint32_t values[19];
	const size_t group_count = ARRAY_SIZE(values);

	for (i = 0; i < group_count; ++i) {
		WREG32_FIELD15(GC, 0, TA_DEBUG_INDEX, INDEX, i);

		udelay(50);

		values[i] = RREG32_SOC15(GC, 0, mmTA_DEBUG_DATA);
	}

	r = sgpu_bpmd_layout_dump_reg32_multiple_read_packet(adev, sbo, domain,
						SOC15_REG_OFFSET(GC, 0, mmTA_DEBUG_INDEX),
						SOC15_REG_OFFSET(GC, 0, mmTA_DEBUG_DATA),
						group_count,
						values, 0);

	if (r == SGPU_BPMD_LAYOUT_INVALID_PACKET_ID) {
		return -ENOMEM;
	}

	return 0;
}

static int gfx_v10_0_bpmd_dump_reg_tcp(struct sgpu_bpmd_output *sbo,
				       struct amdgpu_device *adev)
{
	int r = 0;
	size_t i = 0;
	const u32 domain = SGPU_BPMD_LAYOUT_REG_DOMAIN_SFR;
	uint32_t values[3];
	const size_t group_count = ARRAY_SIZE(values);

	for (i = 0; i < group_count; ++i) {
		WREG32_FIELD15(GC, 0, TCP_DEBUG_INDEX, INDEX, i);

		udelay(50);

		values[i] = RREG32_SOC15(GC, 0, mmTCP_DEBUG_DATA);
	}

	r = sgpu_bpmd_layout_dump_reg32_multiple_read_packet(adev, sbo, domain,
						SOC15_REG_OFFSET(GC, 0, mmTCP_DEBUG_INDEX),
						SOC15_REG_OFFSET(GC, 0, mmTCP_DEBUG_DATA),
						group_count,
						values, 0);
	if (r == SGPU_BPMD_LAYOUT_INVALID_PACKET_ID) {
		return -ENOMEM;
	}

	return 0;
}

static int gfx_v10_0_bpmd_dump_reg_td(struct sgpu_bpmd_output *sbo,
				      struct amdgpu_device *adev)
{
	int r = 0;
	size_t i = 0;
	const u32 domain = SGPU_BPMD_LAYOUT_REG_DOMAIN_SFR;
	uint32_t values[37];
	const size_t group_count = ARRAY_SIZE(values);

	for (i = 0; i < group_count; ++i) {
		WREG32_FIELD15(GC, 0, TD_DEBUG_INDEX, INDEX, i);

		udelay(50);

		values[i] = RREG32_SOC15(GC, 0, mmTD_DEBUG_DATA);
	}

	r = sgpu_bpmd_layout_dump_reg32_multiple_read_packet(adev, sbo, domain,
						SOC15_REG_OFFSET(GC, 0, mmTD_DEBUG_INDEX),
						SOC15_REG_OFFSET(GC, 0, mmTD_DEBUG_DATA),
						group_count,
						values, 0);
	if (r == SGPU_BPMD_LAYOUT_INVALID_PACKET_ID) {
		return -ENOMEM;
	}

	return 0;
}

static u32 gfx_v10_0_grbm_sh_index(u32 cu, u32 simd) {
	return (0x3 & simd) | (0x1c & (cu << 2));
}

static const u32 halt_all_waves_sq_cmd(void)
{
	u32 sq_cmd = 0;

	/* Send SQ the command SQ_IND_CMD_CMD_SETHALT */
	sq_cmd = REG_SET_FIELD(sq_cmd, SQ_CMD, CMD, 0x01);

	/* SQ_IND_CMD_MODE_BROADCAST : Send command to all waves */
	sq_cmd = REG_SET_FIELD(sq_cmd, SQ_CMD, MODE, 0x01);

	/* Send to waves of any VMID */
	sq_cmd = REG_SET_FIELD(sq_cmd, SQ_CMD, CHECK_VMID, 0);

	return sq_cmd;
}

/**
 * @halt: True to halt, False to resume
 */
static void halt_all_waves(struct amdgpu_device *adev, bool halt)
{
	const u32 init_sq_cmd = halt_all_waves_sq_cmd();
	u32 sq_cmd = REG_SET_FIELD(init_sq_cmd, SQ_CMD, DATA, halt ? 0x01 : 0x00);

	WREG32_SOC15(GC, 0, mmSQ_CMD, sq_cmd);
}

static int gfx_v10_0_bpmd_dump_reg_sq(struct sgpu_bpmd_output *sbo,
				      struct amdgpu_device *adev)
{
	size_t i = 0;
	int r = 0;
	int error = 0;
	const uint32_t domain = SGPU_BPMD_LAYOUT_REG_DOMAIN_SFR;
	/* @todo GFXSW-5132 - Update to a header containing this offset */
	const u32 ixSQ_WAVE_SGPR0 = 0x0200;

	struct sgpu_bpmd_layout_reg32 per_sq_reg32[] = {
		{ .index = ixSQ_DEBUG_STS_LOCAL },
	};
	struct sgpu_bpmd_layout_reg32 per_wave_reg32[] = {
		{ .index = ixSQ_WAVE_VALID_AND_IDLE },
		{ .index = ixSQ_WAVE_STATUS },
		{ .index = ixSQ_WAVE_IB_STS },
		{ .index = ixSQ_WAVE_IB_STS2 },
		{ .index = ixSQ_WAVE_IB_DBG1 },
		{ .index = ixSQ_WAVE_PC_LO },
		{ .index = ixSQ_WAVE_PC_HI },
		{ .index = ixSQ_WAVE_EXEC_LO },
		{ .index = ixSQ_WAVE_EXEC_HI },
		{ .index = ixSQ_WAVE_M0 },
	};
	struct sgpu_bpmd_layout_reg32 all_sgpr_reg32[108];

	size_t se = 0;
	size_t sh = 0;   /* sh = sa */
	size_t cu = 0;   /* cu = wgp */
	size_t simd = 0; /* simd = sq */
	size_t wave = 0;

	mutex_lock(&adev->grbm_idx_mutex);
	for (se = 0; se < adev->gfx.config.max_shader_engines; se++) {
	for (sh = 0; sh < adev->gfx.config.max_sh_per_se; sh++) {
	for (cu = 0; cu < adev->gfx.config.max_cu_per_sh; cu++) {
	for (simd = 0; simd < NUM_SIMD_PER_CU; simd++) {
		u32 instance_index = gfx_v10_0_grbm_sh_index(cu, simd);
		u32 grbm_gfx_index = gfx_v10_0_select_se_sh(adev, se, sh,
							    instance_index);

		DRM_DEBUG("cu %zu simd %zu se %zu sh %zu\n", cu, simd, se, sh);
		DRM_DEBUG("instance_index : %u\n", instance_index);
		DRM_DEBUG("grbm_gfx_index : %u\n", grbm_gfx_index);

		halt_all_waves(adev, true);
		udelay(50);

		for (i = 0; i < ARRAY_SIZE(per_sq_reg32); i++) {
			per_sq_reg32[i].value = wave_read_ind(
				adev, 0, per_sq_reg32[i].index);
		}
		r = sgpu_bpmd_layout_dump_reg32_packet(adev, sbo, domain,
			ARRAY_SIZE(per_sq_reg32), per_sq_reg32, grbm_gfx_index);
		if (r == SGPU_BPMD_LAYOUT_INVALID_PACKET_ID) {
			error = -ENOMEM;
			goto unlock;
		}

		for (wave = 0; wave < adev->gfx.cu_info.max_waves_per_simd; wave++) {
			u32 idle_waves_mask = wave_read_ind(adev, wave, ixSQ_WAVE_VALID_AND_IDLE);
			per_wave_reg32[0].value = idle_waves_mask;

			if ((idle_waves_mask >> wave) & 0x1) {
				for (i = 1; i < ARRAY_SIZE(per_wave_reg32); i++) {
					per_wave_reg32[i].value = wave_read_ind(
						adev,
						wave,
						per_wave_reg32[i].index);
				}
			}

			r = sgpu_bpmd_layout_dump_indexed_reg32_packet(
				adev, sbo, domain,
				ARRAY_SIZE(per_wave_reg32),
				per_wave_reg32, grbm_gfx_index,
				SOC15_REG_OFFSET(GC, 0, mmSQ_IND_INDEX));
			if (r == SGPU_BPMD_LAYOUT_INVALID_PACKET_ID) {
				error = -ENOMEM;
				goto unlock;
			}

			/* Auto increment the couter to read every SGPR registers */
			WREG32_SOC15(GC, 0, mmSQ_IND_INDEX,
				(wave << SQ_IND_INDEX__WAVE_ID__SHIFT) |
				(ixSQ_WAVE_SGPR0 << SQ_IND_INDEX__INDEX__SHIFT) |
				(SQ_IND_INDEX__AUTO_INCR_MASK));
			for (i = 0; i < ARRAY_SIZE(all_sgpr_reg32); ++i) {
				all_sgpr_reg32[i].index = ixSQ_WAVE_SGPR0 + i;
				all_sgpr_reg32[i].value = RREG32_SOC15(GC, 0, mmSQ_IND_DATA);
			}
			r = sgpu_bpmd_layout_dump_indexed_reg32_packet(adev, sbo, domain,
				ARRAY_SIZE(all_sgpr_reg32), all_sgpr_reg32, grbm_gfx_index,
				SOC15_REG_OFFSET(GC, 0, mmSQ_IND_INDEX));
			if (r == SGPU_BPMD_LAYOUT_INVALID_PACKET_ID) {
				error = -ENOMEM;
				goto unlock;
			}
		}

		/* Restore the state:
		 * Resume the waves that were halted in order to dump the registers
		 */
		halt_all_waves(adev, false);
	}
	}
	}
	}

unlock:
	gfx_v10_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);
	mutex_unlock(&adev->grbm_idx_mutex);

	return error;
}

static int gfx_v10_0_bpmd_dump_reg_msys(struct sgpu_bpmd_output *sbo,
					struct amdgpu_device *adev)
{
	size_t i = 0;
	size_t j = 0;
	int r = 0;
	struct sgpu_bpmd_layout_reg32 reg32[] = {
		/* GL1C (4 instances):  */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_UTCL0_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER0_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER0_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER0_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER1_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER1_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER1_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER2_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER2_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER2_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER3_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER3_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_PERFCOUNTER3_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_CTRL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_CTRL2) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_UTCL0_CNTL1) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_UTCL0_CNTL2) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1C_UTCL0_RETRY) },

		/* CHC (4 instances): */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_CTRL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER0_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER0_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER0_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER1_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER1_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER1_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER2_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER2_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER2_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER3_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER3_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHC_PERFCOUNTER3_SELECT) },

		/* GL2 (4 instances): */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL2A_CTRL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL2C_CTRL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL2C_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL2C_CM_CTRL2) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL2C_CTRL3) },
		/* Doesn't exist without a number at the end */
		/* { .index = SOC15_REG_OFFSET(GC, 0, mmGL2C_PERFCOUNTER) }, */

		/* ACEM (4 instances): */
		/* @todo GFXSW-5132 - Update to a header containing this offset
		 *
		 * Not part of gc_10_3_0_offset.h, but as part of gc_10_4_0_offset.h.
		 * Are we expecting those files to be updated?
		*/
		/* { .index = SOC15_REG_OFFSET(GC, 0, mmGL2ACEM_CTRL0) }, */
		/* { .index = SOC15_REG_OFFSET(GC, 0, mmGL2ACEM_STUS) }, */
	};
	const size_t num_regs = ARRAY_SIZE(reg32);
	const uint32_t domain = SGPU_BPMD_LAYOUT_REG_DOMAIN_SFR;
	const size_t instance_count = 4;
	int error = 0;

	mutex_lock(&adev->grbm_idx_mutex);
	for (i = 0; i < instance_count; i++) {
		u32 grbm_gfx_index = gfx_v10_0_select_se_sh(adev, 0, 0, i);
		for (j = 0; j < num_regs; j++) {
			reg32[j].value = RREG32(reg32[j].index);
		}
		r = sgpu_bpmd_layout_dump_reg32_packet(adev, sbo, domain, num_regs, reg32, grbm_gfx_index);
		if (r == SGPU_BPMD_LAYOUT_INVALID_PACKET_ID) {
			error = -ENOMEM;
			break;
		}
	}
	gfx_v10_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);
	mutex_unlock(&adev->grbm_idx_mutex);

	return error;
}

/**
 * read_cp_cmds - Dumps the CMD_INDEX register as well as the CMD_DATA
 * 		  associated with the index
 *
 * @index: The CMD_INDEX to dump
 * @reg32: A pointer to the array where to record the register's values.
 * 	   This array must have at least dw_count + 1 allocated DWords.
 * @dw_count: Number of dw to read for CMD_DATA
 *
 * Return: Number of registers read
 */
static size_t read_cp_cmds(struct amdgpu_device *adev,
			   u32 index,
			   struct sgpu_bpmd_layout_reg32 *reg32,
			   size_t dw_count)
{
	size_t i = 0;

	/* Start from index 0 within the Queue */
	index = REG_SET_FIELD(index, CP_CMD_INDEX, CMD_INDEX, 0);

	/* Record CMD_INDEX's value and select the right index */
	reg32[0].index = SOC15_REG_OFFSET(GC, 0, mmCP_CMD_INDEX);
	reg32[0].value = index;
	WREG32(reg32[0].index, index);
	udelay(50);

	/* Record the data section */
	for (i = 1; i < dw_count + 1; i++) {
		reg32[i].index = SOC15_REG_OFFSET(GC, 0, mmCP_CMD_DATA);
		reg32[i].value = RREG32(reg32[i].index);
	}

	return i;
}

static int gfx_v10_0_bpmd_dump_reg_cp_cmd(struct sgpu_bpmd_output *sbo,
					  struct amdgpu_device *adev)
{
	const uint32_t domain = SGPU_BPMD_LAYOUT_REG_DOMAIN_SFR;
	const size_t hqd_roq_size = 1280;
	const size_t pfp_roq_size = 1280;
	const size_t meq_size = 512;
	const size_t stq_size = 512;
	const size_t queue_count = 4;
	const size_t total_size =
		hqd_roq_size + pfp_roq_size + meq_size + stq_size + queue_count;
	int r = 0;
	u32 index = 0;
	enum QUEUE_SEL {
		HQD_ROQ = 0b000,
		PFP_ROQ = 0b001,
		MEQ     = 0b010,
		STQ     = 0b110,
	};
	struct sgpu_bpmd_layout_reg32 *reg32 = kmalloc(
		sizeof(struct sgpu_bpmd_layout_reg32) * total_size, GFP_ATOMIC);
	struct sgpu_bpmd_layout_reg32 *reg32_pos = reg32;

	if (!reg32) {
		return -ENOMEM;
	}

	index = REG_SET_FIELD(index, CP_CMD_INDEX, CMD_ME_SEL,    1);
	index = REG_SET_FIELD(index, CP_CMD_INDEX, CMD_QUEUE_SEL, HQD_ROQ);
	reg32_pos += read_cp_cmds(adev, index, reg32_pos, hqd_roq_size);

	index = REG_SET_FIELD(index, CP_CMD_INDEX, CMD_ME_SEL,    0);
	index = REG_SET_FIELD(index, CP_CMD_INDEX, CMD_QUEUE_SEL, PFP_ROQ);
	reg32_pos += read_cp_cmds(adev, index, reg32_pos, pfp_roq_size);

	index = REG_SET_FIELD(index, CP_CMD_INDEX, CMD_QUEUE_SEL, MEQ);
	reg32_pos += read_cp_cmds(adev, index, reg32_pos, meq_size);

	index = REG_SET_FIELD(index, CP_CMD_INDEX, CMD_QUEUE_SEL, STQ);
	reg32_pos += read_cp_cmds(adev, index, reg32_pos, stq_size);

	r = sgpu_bpmd_layout_dump_reg32_packet(
		adev, sbo, domain, total_size, reg32, 0);

	kfree(reg32);

	return r == SGPU_BPMD_LAYOUT_INVALID_PACKET_ID ? -ENOMEM : 0;
}

/** @todo GFXSW-5224 - Enable the below function when it is guaranteed for the
 * GPU to be resetting after BPMD dump finished */
#if 0
static int gfx_v10_0_bpmd_dump_reg_grbm_debug(struct sgpu_bpmd_output *sbo,
					      struct amdgpu_device *adev)
{
	int r = 0;
	const uint32_t domain = SGPU_BPMD_LAYOUT_REG_DOMAIN_SFR;
	const size_t num_regs = 1;
	struct sgpu_bpmd_layout_reg32 reg32 =
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_DEBUG_SNAPSHOT) };

	/* Setting INGORE_RDY to 1 is required to get the SNAPSHOT.
	 * However, setting it back to 0 hasn't been tested in DV, so this
	 * entire function should only be used when a GPU reset is bound to
	 * happen after the dump */
	WREG32_FIELD15(GC, 0, GRBM_DEBUG, IGNORE_RDY, 1);
	reg32.value = RREG32(reg32.index);

	r = sgpu_bpmd_layout_dump_reg32_packet(adev, sbo, domain, num_regs, &reg32, 0);
	if (r == SGPU_BPMD_LAYOUT_INVALID_PACKET_ID) {
		return -ENOMEM;
	}

	return 0;
}
#endif

#define SGPU_BPMD_DUMP_REG(UNIT, sbo, adev) 					\
({										\
	int __dump_reg_ret = gfx_v10_0_bpmd_dump_reg_##UNIT (sbo, adev);		\
	if (__dump_reg_ret) {							\
		DRM_INFO("Failed dumping registers for " #UNIT " with err %d",	\
			 r);							\
	}									\
	(__dump_reg_ret);							\
})

static int gfx_v10_0_bpmd_dump_reg(struct sgpu_bpmd_output *sbo,
				   struct amdgpu_device *adev)
{
	uint32_t i = 0;
	int r = 0;
	const uint32_t domain = SGPU_BPMD_LAYOUT_REG_DOMAIN_SFR;
	struct sgpu_bpmd_layout_reg32 reg32[] = {
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCPC_UTCL1_ERROR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCPC_UTCL1_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCPF_UTCL1_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCPG_UTCL1_ERROR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCPG_UTCL1_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_BUSY_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_CNTX_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_CPC_BUSY_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_CPC_STALLED_STAT1) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_CPC_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_CPF_BUSY_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_CPF_STALLED_STAT1) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_CPF_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_CSF_STAT ) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HPD_STATUS0) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_HQ_STATUS0) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_WPTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GRBM_FREE_COUNT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_GFX_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_CNTL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_ME_CNTL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_PQ_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB0_BASE) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB0_BASE_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB0_CNTL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB0_RPTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB0_WPTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB0_WPTR_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB1_BASE) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB1_BASE_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB1_CNTL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB1_RPTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB1_WPTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB1_WPTR_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_RB_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_STALLED_STAT1) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_STALLED_STAT2) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_STALLED_STAT3) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_VMID_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCVM_CONTEXT0_CNTL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCVM_L2_PROTECTION_FAULT_ADDR_HI32)},
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCVM_L2_PROTECTION_FAULT_ADDR_LO32)},
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCVM_L2_PROTECTION_FAULT_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_CHIP_REVISION) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_READ_ERROR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_READ_ERROR2) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_RSMU_READ_ERROR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS2) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS3) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS_SE0) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS_SE1) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS_SE2) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS_SE3) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGRBM_WRITE_ERROR) },
		/* RLC */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmRLC_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmRLC_INT_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmRLC_GPM_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmRLC_F32_UCODE_VERSION) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmRLC_SAFE_MODE) },
		/* GL1/GL1A */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1_ARB_CTRL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1_ARB_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1_PIPE_STEER) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER0_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER0_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER0_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER1_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER1_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER1_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER2_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER2_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER2_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER3_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER3_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_PERFCOUNTER3_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_GL1C_CREDITS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGL1A_CLIENT_FREE_DELAY) },
		/* CH/CHA */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCH_ARB_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_CHC_CREDITS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_CLIENT_FREE_DELAY) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCH_ARB_CTRL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCH_PIPE_STEER) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER0_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER0_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER0_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER1_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER1_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER1_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER2_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER2_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER2_SELECT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER3_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER3_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCHA_PERFCOUNTER3_SELECT) },
		/* GCR */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCR_GENERAL_CNTL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCR_CMD_STATUS) },
		/* UTCL1 */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmUTCL1_CTRL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmUTCL1_STATUS) },
		/* UTCL2 */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCUTCL2_PERFCOUNTER_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCUTCL2_PERFCOUNTER_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCVM_L2_CGTT_CLK_CTRL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCUTCL2_CGTT_CLK_CTRL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmGCVM_L2_STATUS) },
		/* SQ */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmSQ_DEBUG_STS_GLOBAL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmSQ_DEBUG_STS_GLOBAL2) },
	};
	const uint32_t num_regs = ARRAY_SIZE(reg32);

	if (adev->ddev.switch_power_state == DRM_SWITCH_POWER_OFF) {
		DRM_INFO("%s: Skip dumping register due to GPU power off",
			 __func__);
		return SGPU_BPMD_LAYOUT_INVALID_PACKET_ID;
	}

	for (i = 0; i < num_regs; i++) {
		reg32[i].value = RREG32(reg32[i].index);
	}

	r = sgpu_bpmd_layout_dump_reg32_packet(adev, sbo, domain, num_regs, reg32, 0);
	if (r == SGPU_BPMD_LAYOUT_INVALID_PACKET_ID) {
		return -ENOMEM;
	}

	r = SGPU_BPMD_DUMP_REG(msys, sbo, adev);
	if (r)
		return r;
	r = SGPU_BPMD_DUMP_REG(ta, sbo, adev);
	if (r)
		return r;
	r = SGPU_BPMD_DUMP_REG(tcp, sbo, adev);
	if (r)
		return r;
	r = SGPU_BPMD_DUMP_REG(td, sbo, adev);
	if (r)
		return r;
	r = SGPU_BPMD_DUMP_REG(sq, sbo, adev);
	if (r)
		return r;
	if (SGPU_BPMD_ENABLE_UNVERIFIED) {
		r = SGPU_BPMD_DUMP_REG(cp_cmd, sbo, adev);
		if (r)
			return r;
	}
	/* @todo GFXSW-5224 - Enable only when BPMD is trigerred in a way that
	   there is no guaranttee about the stability of the system afterwards

	 * r = SGPU_BPMD_DUMP_REG(grbm_debug, sbo, adev);
	 */

	return r;
}

static uint32_t gfx_v10_0_bpmd_dump_gfx_ring_reg32(struct sgpu_bpmd_output *sbo,
					       struct amdgpu_ring *ring)
{
	uint32_t i = 0;
	struct amdgpu_device *adev = ring->adev;
	const uint32_t vmid = 0;
	const uint32_t domain = SGPU_BPMD_LAYOUT_REG_DOMAIN_SFR;
	struct sgpu_bpmd_layout_reg32 reg32[] = {
		/* CP */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCPC_INT_STATUS) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_CPC_PRIV_VIOLATION_ADDR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_ERROR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_ACTIVE) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_BASE) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_BASE_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_CNTL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_CSMD_RPTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_OFFSET) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_RPTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_VMID) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_ACTIVE) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_ERROR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB1_BASE_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB1_BASE_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB1_BUFSZ) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB1_CMD_BUFSZ) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB1_OFFSET) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB1_PREAMBLE_BEGIN) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB1_PREAMBLE_END) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB2_BASE_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB2_BASE_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB2_BUFSZ) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB2_CMD_BUFSZ) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB2_OFFSET) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB2_PREAMBLE_BEGIN) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_IB2_PREAMBLE_END) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_INT_STAT_DEBUG) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEQ_AVAIL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEQ_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_ME_INSTR_PNTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_PFP_INSTR_PNTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_PRIV_VIOLATION_ADDR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_ROQ2_AVAIL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_ROQ_AVAIL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_ROQ_DB_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_ROQ_IB1_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_ROQ_IB2_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_ROQ_RB_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_STQ_STAT) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_STQ_WR_STAT) },
		/* @todo GFXSW-4705 - Per pipe */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_INT_STATUS) },
	};
	const uint32_t num_regs = ARRAY_SIZE(reg32);
	int r = 0;
	uint32_t CP_PFP_HEADER_DUMP_values[CP_PFP_HEADER_DUMP_reg_read_count] = {0};
	uint32_t CP_ME_HEADER_DUMP_values[CP_ME_HEADER_DUMP_reg_read_count]  = {0};

	if (adev->ddev.switch_power_state == DRM_SWITCH_POWER_OFF) {
		DRM_INFO("%s: Skip dumping register due to GPU power off",
			 __func__);
		return SGPU_BPMD_LAYOUT_INVALID_PACKET_ID;
	}

	r = mutex_lock_interruptible(&adev->srbm_mutex);
	gfx_v10_0_select_me_pipe_q(adev, ring->me, ring->pipe, ring->queue,
				   vmid);

	for (i = 0; i < num_regs; i++) {
		reg32[i].value = RREG32(reg32[i].index);
	}

	for (i = 0; i < CP_PFP_HEADER_DUMP_reg_read_count; i++) {
		CP_PFP_HEADER_DUMP_values[i] = RREG32_SOC15(GC, 0, mmCP_PFP_HEADER_DUMP);
	}

	for (i = 0; i < CP_ME_HEADER_DUMP_reg_read_count; i++) {
		CP_ME_HEADER_DUMP_values[i] = RREG32_SOC15(GC, 0, mmCP_ME_HEADER_DUMP);
	}

	gfx_v10_0_select_me_pipe_q(adev, 0, 0, 0, 0); /* set to default */
	mutex_unlock(&adev->srbm_mutex);

	sgpu_bpmd_layout_dump_reg32_multiple_read_packet(adev, sbo, domain, 0,
							 SOC15_REG_OFFSET(GC, 0, mmCP_PFP_HEADER_DUMP),
							 CP_PFP_HEADER_DUMP_reg_read_count,
							 CP_PFP_HEADER_DUMP_values, 0);

	sgpu_bpmd_layout_dump_reg32_multiple_read_packet(adev, sbo, domain, 0,
							 SOC15_REG_OFFSET(GC, 0, mmCP_ME_HEADER_DUMP),
							 CP_ME_HEADER_DUMP_reg_read_count,
							 CP_ME_HEADER_DUMP_values, 0);

	return sgpu_bpmd_layout_dump_reg32_packet(adev, sbo, domain, num_regs, reg32, 0);
}

static uint32_t gfx_v10_0_bpmd_dump_compute_ring_reg32(struct sgpu_bpmd_output *sbo,
						       struct amdgpu_ring *ring)
{
	uint32_t i = 0;
	struct amdgpu_device *adev = ring->adev;
	const uint32_t vmid = 0;
	const uint32_t domain = SGPU_BPMD_LAYOUT_REG_DOMAIN_SFR;
	struct sgpu_bpmd_layout_reg32 reg32[] = {
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_BASE_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_BASE) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_WPTR_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_WPTR_LO) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_CONTROL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_IB_BASE_ADDR_HI) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_IB_BASE_ADDR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_IB_CONTROL) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_IB_RPTR) },

		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC1_INSTR_PNTR) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC2_INSTR_PNTR) },
		/* ME Last 8 decoded header */
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME1_HEADER_DUMP) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME1_HEADER_DUMP) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME1_HEADER_DUMP) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME1_HEADER_DUMP) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME1_HEADER_DUMP) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME1_HEADER_DUMP) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME1_HEADER_DUMP) },
		{ .index = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_ME1_HEADER_DUMP) },
	};
	const uint32_t num_regs = ARRAY_SIZE(reg32);
	int r = 0;

	if (adev->ddev.switch_power_state == DRM_SWITCH_POWER_OFF) {
		DRM_INFO("%s: Skip dumping register due to GPU power off",
			 __func__);
		return SGPU_BPMD_LAYOUT_INVALID_PACKET_ID;
	}

	r = mutex_lock_interruptible(&adev->srbm_mutex);
	gfx_v10_0_select_me_pipe_q(adev, ring->me, ring->pipe, ring->queue,
				   vmid);

	for (i = 0; i < num_regs; i++) {
		reg32[i].value = RREG32(reg32[i].index);
	}

	gfx_v10_0_select_me_pipe_q(adev, 0, 0, 0, 0); /* set to default */
	mutex_unlock(&adev->srbm_mutex);

	return sgpu_bpmd_layout_dump_reg32_packet(adev, sbo, domain, num_regs, reg32, 0);
}

static uint32_t gfx_v10_0_bpmd_dump_ring(struct sgpu_bpmd_output *sbo,
					 struct amdgpu_ring *ring)
{
	uint32_t reg32_packet_id = SGPU_BPMD_LAYOUT_INVALID_PACKET_ID;

	switch(ring->funcs->type) {
	case AMDGPU_RING_TYPE_GFX:
		reg32_packet_id =
			gfx_v10_0_bpmd_dump_gfx_ring_reg32(sbo, ring);
		break;
	case AMDGPU_RING_TYPE_COMPUTE:
		reg32_packet_id =
			gfx_v10_0_bpmd_dump_compute_ring_reg32(sbo, ring);
		break;
	default:
		break;
	}

	return sgpu_bpmd_layout_dump_ring_packet(sbo, ring, reg32_packet_id);
}

static void gfx_v10_0_bpmd_find_ibs(const uint32_t *addr, uint32_t size,
				    uint32_t vmid, struct list_head *list)
{
	static const uint32_t count_bias = 2; /* 1 for header + 1 for count */
	const uint32_t *start = addr;
	const uint32_t *end = (uint32_t *)((uint8_t *)addr + size);

	while (start < end) {
		const uint32_t opcode = CP_PACKET3_GET_OPCODE(start[0]);
		uint32_t count = CP_PACKET_GET_COUNT(start[0]);

		BUG_ON(CP_PACKET_GET_TYPE(start[0]) != PACKET_TYPE3);

		DRM_DEBUG("%s: opcode %x, count %x\n", __func__, opcode, count);

		if (opcode == PACKET3_INDIRECT_BUFFER) {
			const uint64_t ib_gpu_addr =
				(INDIRECT_BUFFER_GET_GPU_ADDR_HI(start[2]) |
				 INDIRECT_BUFFER_GET_GPU_ADDR_LO(start[1])) &
				AMDGPU_GMC_HOLE_MASK;
			static const uint32_t dw_byte = 4;
			const uint64_t ib_size =
				INDIRECT_BUFFER_GET_SIZE(start[3]) * dw_byte;
			const uint64_t ib_vmid =
				(vmid == SGPU_BPMD_INVALID_VMID) ?
				INDIRECT_BUFFER_GET_VMID(start[3]) : vmid;

			if (sgpu_bpmd_add_ib_info(list, ib_gpu_addr, ib_size,
						  ib_vmid) == false)
				return;
		}

		if ((opcode == PACKET3_NOP) && (count == 0x3FFF))
			count = 1;
		else
			count += count_bias;
		start += count;
	}
}

static void gfx_v10_0_set_irq_funcs(struct amdgpu_device *adev)
{
	adev->gfx.eop_irq.num_types = AMDGPU_CP_IRQ_LAST;
	adev->gfx.eop_irq.funcs = &gfx_v10_0_eop_irq_funcs;

	adev->gfx.kiq.irq.num_types = AMDGPU_CP_KIQ_IRQ_LAST;
	adev->gfx.kiq.irq.funcs = &gfx_v10_0_kiq_irq_funcs;

	adev->gfx.priv_reg_irq.num_types = 1;
	adev->gfx.priv_reg_irq.funcs = &gfx_v10_0_priv_reg_irq_funcs;

	adev->gfx.priv_inst_irq.num_types = 1;
	adev->gfx.priv_inst_irq.funcs = &gfx_v10_0_priv_inst_irq_funcs;
}

static void gfx_v10_0_set_rlc_funcs(struct amdgpu_device *adev)
{
	adev->gfx.rlc.funcs = &gfx_v10_0_rlc_funcs;
}

static void gfx_v10_0_set_bpmd_funcs(struct amdgpu_device *adev)
{
#ifdef CONFIG_DRM_SGPU_BPMD
	adev->bpmd.funcs = &gfx_v10_0_bpmd_funcs;
#endif  /* CONFIG_DRM_SGPU_BPMD */
}

static void gfx_v10_0_set_gds_init(struct amdgpu_device *adev)
{
	unsigned total_cu = adev->gfx.config.max_cu_per_sh *
			    adev->gfx.config.max_sh_per_se *
			    adev->gfx.config.max_shader_engines;

	if (adev->asic_type == CHIP_VANGOGH_LITE)
		return;

	adev->gds.gds_size = 0x10000;
	adev->gds.gds_compute_max_wave_id = total_cu * 32 - 1;
	adev->gds.gws_size = 64;
	adev->gds.oa_size = 16;
}

static u32 gfx_v10_0_get_wgp_active_bitmap_per_sh(struct amdgpu_device *adev)
{
	u32 data, wgp_bitmask;
	data = RREG32_SOC15(GC, 0, mmCC_GC_SHADER_ARRAY_CONFIG);
	data |= RREG32_SOC15(GC, 0, mmGC_USER_SHADER_ARRAY_CONFIG);

	data &= CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK;
	data >>= CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT;

	wgp_bitmask =
		amdgpu_gfx_create_bitmask(adev->gfx.config.max_cu_per_sh >> 1);

	return (~data) & wgp_bitmask;
}


static int gfx_v10_0_get_cu_info(struct amdgpu_device *adev,
				 struct amdgpu_cu_info *cu_info)
{
	int i, j, k, counter, active_cu_number = 0;
	u32 mask, bitmap, ao_bitmap, ao_cu_mask = 0;
	unsigned disable_masks[4 * 2];

	if (!adev || !cu_info)
		return -EINVAL;

	amdgpu_gfx_parse_disable_cu(disable_masks, 4, 2);

	mutex_lock(&adev->grbm_idx_mutex);
	for (i = 0; i < adev->gfx.config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx.config.max_sh_per_se; j++) {
			bitmap = i * adev->gfx.config.max_sh_per_se + j;
			mask = 1;
			ao_bitmap = 0;
			counter = 0;
			gfx_v10_0_select_se_sh(adev, i, j, 0xffffffff);
			if (i < 4 && j < 2)
				gfx_v10_0_set_user_wgp_inactive_bitmap_per_sh(
					adev, disable_masks[i * 2 + j]);
			bitmap = gfx_v10_0_get_cu_active_bitmap_per_sh(adev);
			cu_info->bitmap[i][j] = bitmap;

			for (k = 0; k < adev->gfx.config.max_cu_per_sh; k++) {
				if (bitmap & mask) {
					if (counter < adev->gfx.config.max_cu_per_sh)
						ao_bitmap |= mask;
					counter++;
				}
				mask <<= 1;
			}
			active_cu_number += counter;
			if (i < 2 && j < 2)
				ao_cu_mask |= (ao_bitmap << (i * 16 + j * 8));
			cu_info->ao_cu_bitmap[i][j] = ao_bitmap;
		}
	}
	gfx_v10_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);
	mutex_unlock(&adev->grbm_idx_mutex);

	cu_info->number = active_cu_number;
	cu_info->ao_cu_mask = ao_cu_mask;
	cu_info->simd_per_cu = NUM_SIMD_PER_CU;

	return 0;
}

static void gfx_v10_0_static_wgp_clockgating_init(struct amdgpu_device *adev)
{
	int i, j;

	/* 1 WGP means 2 CUs */
	adev->gfx.num_wgps = adev->gfx.cu_info.number >> 1;
	adev->gfx.wgp_bitmask =
		amdgpu_gfx_create_bitmask(adev->gfx.config.max_cu_per_sh >> 1);
	adev->gfx.num_clock_on_wgp = adev->gfx.num_wgps;
	adev->gfx.num_aon_wgp = 0;

	mutex_lock(&adev->grbm_idx_mutex);
	for (i = 0; i < adev->gfx.config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx.config.max_sh_per_se; j++) {
			gfx_v10_0_select_se_sh(adev, i, j, 0xffffffff);
			adev->gfx.wgp_active_bitmap[i][j] =
					gfx_v10_0_get_wgp_active_bitmap_per_sh(adev);
			adev->gfx.wgp_aon_bitmap[i][j] = 0;
		}
	}
	gfx_v10_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);
	mutex_unlock(&adev->grbm_idx_mutex);
}

const struct amdgpu_ip_block_version gfx_v10_0_ip_block =
{
	.type = AMD_IP_BLOCK_TYPE_GFX,
	.major = 10,
	.minor = 0,
	.rev = 0,
	.funcs = &gfx_v10_0_ip_funcs,
};
