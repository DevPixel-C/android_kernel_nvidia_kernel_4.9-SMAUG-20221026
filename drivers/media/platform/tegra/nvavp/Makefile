GCOV_PROFILE := y
ccflags-y += -I../nvidia/drivers/video/tegra/host

ccflags-y += -Werror
obj-$(CONFIG_TEGRA_NVAVP)		+= nvavp_dev.o
obj-$(CONFIG_TEGRA_AVP_KERNEL_ON_MMU)	+= ../avp/headavp.o
