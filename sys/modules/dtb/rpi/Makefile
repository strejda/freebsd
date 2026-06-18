# DTS files for the 32-bit and 64-bit arm-based Raspberry Pi family of devices
.if ${MACHINE_CPUARCH} == "arm"
DTSO=	\
	spigen-rpi2.dtso
.elif ${MACHINE_CPUARCH} == "aarch64"
DTS=						\
	broadcom/bcm2712-d-rpi-5-b.dts		\
	broadcom/bcm2712-rpi-5-b.dts

DTSO=	\
	spigen-rpi3.dtso \
	spigen-rpi4.dtso
.endif

.include <bsd.dtb.mk>
