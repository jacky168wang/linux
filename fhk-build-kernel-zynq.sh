#!/bin/sh

#arm_cc
#export ARCH=arm
#export CROSS_COMPILE=arm-linux-gnueabihf-

#cd linux-adi-fhk

#git checkout facc_altera4.9
#make facc_a10ad9371_defconfig O=../out-linux-a10ad9371
#make facc-a10ad9371x2.dtb O=../out-linux-a10ad9371
#make O=../out-linux-a10ad9371

#git checkout adrv9009_zc706
#OUT_DIR=../out-adi-zc706ad9009
#make zynq_xcomm_adv7511_defconfig O=$OUT_DIR
#make zynq-zc706-adv7511-adrv9009.dtb O=$OUT_DIR
#make -j5 UIMAGE_LOADADDR=0x8000 uImage O=$OUT_DIR

#git checkout facc_zc706ad9009
OUT_DIR=../out-fhk-zc706ad9009 # to be changed into "out-fhk_"
make fhk_zc706ad9009_defconfig O=$OUT_DIR
#make fhk_zc706ad9009.dtb O=$OUT_DIR # to be changed into "fhk_"
make -j5 UIMAGE_LOADADDR=0x8000 uImage O=$OUT_DIR

#export ARCH=
#export CROSS_COMPILE=
#arm_rm
