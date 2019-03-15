#!/bin/sh

arm_cc
#export ARCH=arm
#export CROSS_COMPILE=arm-linux-gnueabihf-

#cd linux-adi-fhk

#git checkout fhk_altera4.9
#make facc_a10ad9371_defconfig O=../out-linux-a10ad9371
#make facc-a10ad9371x2.dtb O=../out-linux-a10ad9371
#make -j5 O=../out-linux-a10ad9371

#git checkout adrv9009_zc706
#OUT_DIR=../out_adrv9009_zc706
#make zynq_xcomm_adv7511_defconfig O=$OUT_DIR
#make zynq-zc706-adv7511-adrv9009.dtb O=$OUT_DIR
#make -j5 UIMAGE_LOADADDR=0x8000 uImage O=$OUT_DIR

BRANCH=fhk_zc706ad9009
git checkout ${BRANCH}
OUT_DIR=../out_${BRANCH}
make ${BRANCH}_defconfig O=${OUT_DIR}
#make -j5 UIMAGE_LOADADDR=0x8000 uImage O=${OUT_DIR} KCFLAGS=-DDEBUG
make -j5 UIMAGE_LOADADDR=0x8000 uImage O=${OUT_DIR}
make ${BRANCH}.dtb O=${OUT_DIR}

#export ARCH=
#export CROSS_COMPILE=
#arm_rm
