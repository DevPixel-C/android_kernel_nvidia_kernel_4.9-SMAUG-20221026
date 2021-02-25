GCOV_PROFILE := y
ccflags-y += -I../nvidia/drivers/video/tegra/host

ifdef CONFIG_TEGRA_HV_MANAGER
obj-$(CONFIG_TEGRA_NVAVP_VIRT)		+= nvavp_dev_virt.o nvavp_dev_ivc.o
endif
