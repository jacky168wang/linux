#!/bin/sh

#arm_cc
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-

#*******************************************************************************
#                   "fhk_a10ad9371" SDR-system architecture
#*******************************************************************************
#---- ClockPath:
# GNSS-GPS/LMK04828('drivers/misc/ti_lmk04828.c') -> 10MHz + PPS ->
#   AD9548('drivers/iio/frequency/ad9548.c') ->  32.768MHz ->
#   AD9528('drivers/iio/frequency/ad9528.c') -> 122.880MHz x 2 =>
#   DEV_CLK -> AD9371 -> 2 x JESD204B-RX/TX/ORX             -> serdes-lanes --\
#   FMC_CLK -> a10fpll('drivers/clk/clk-altera-a10-fpll.c') -> serdes-lanes --/

#---- SW-DataPath:
# uspace-"iio-stream.elf"
# uspace-"libiio"
# uspace-"/sys/bus/iio/devices/[attributions]"
# kspace-"syscalls"
# kspace-"VFS"
# kspace-"IIO-Subsystem"
#   JESD204B-TX-DMA('dma/dma-axi-dmac.c') ->
#   JESD204B-TX-L3('iio/frequency/cf_axi_dds.c') ->
#   JESD204B-TX-L2('iio/jesd204/axi_jesd204_tx.c') ->
#   JESD204B-L1('iio/jesd204/altera_adxcvr.c')              -> serdes-lanes --\
#   TX-Ant <- TX-RFFE(PA)  <- AD9371: DAC<-FIR<-DeFramerA   <- serdes-lanes --/
#
#   RX-Ant -> RX-RFFE(LNA) -> AD9371: ADC->FIR->FramerA     -> serdes-lanes --\
#   JESD204B-L1('iio/jesd204/altera_adxcvr.c')              <- serdes-lanes --/
#   JESD204B-RX-L2('iio/jesd204/axi_jesd204_rx.c') <-
#   JESD204B-RX-L3('iio/adc/cf_axi_adc_core.c') <-
#   JESD204B-TX-DMA('dma/dma-axi-dmac.c') <-
# kspace-"IIO-Subsystem"
# ...
#
#             OR-RFFE      -> AD9371: ADC->FIR->FramerB     -> serdes-lanes --\
#   JESD204B-L1('iio/jesd204/altera_adxcvr.c')              <- serdes-lanes --/
#   JESD204B-RX-L2('iio/jesd204/axi_jesd204_rx.c') <-
#   JESD204B-OR-L3('iio/adc/ad_adc.c') <-
# kspace-"IIO-Subsystem"
# ...
#

#---- HW-DataPath: 10GEth-Subsytem-Clock('drivers/clk/clk-si5338.c')
# BBU ... eCPRI ... 10GEth_TX =>
# 10GEth-PHY(SERDES => PMA_RX => PCS_RX) => xMII =>
# 10GEth-MAC(MAC_RX) => RX_FIFO =>
#                     /-> C/S-plane -> PL2PS_bridge -> PS_DDR_MEM
# eCPRI_DL_Dispatcher
#                     \-> U-plane(Timing/PUSCH/PDSCH) ->
#                     SC_Map -> Decompress -> IFTT -> CP_Insert -> 122.88MSPS ->
#                     LP -> USC -> CFR -> Gain -> DPD -> 491.56MSPS ->
#                                               JESD204B_TX -> serdes-lanse --\
#   TX-Ant <- TX-RFFE(PA)  <- AD9371: DAC<-FIR<-DeFramerA   <- serdes-lanes --/
#
#   RX-Ant -> RX-RFFE(LNA) -> AD9371: ADC->FIR->FramerA     -> serdes-lanes --\
#                                               JESD204B_RX <- serdes-lanes --/
#                     DSC <- LP <- 245.76MSPS <-
#                     SC_DeMap <- Compress <- FFT <- CP_Remove <- 122.88MSPS <-
#                     /<- U-plane(Timing/PUSCH/PDSCH) <-
# eCPRI_UL_Dispatcher
#                     \<- C/S-plane <- PS2PL_bridge <- PS_DDR_MEM
# GEth-MAC(MAC_TX) <= TX_FIFO <=
# 10GEth-PHY(SERDES <= PMA_TX <= PCS_TX) <= xMII <=
# BBU ... eCPRI ... 10GEth_RX <=
#*******************************************************************************

BRANCH=fhk_a10ad9371
#BRANCH=fhk_zc706ad9009

#git checkout adrv9009_zc706
#OUT_DIR=../out_adrv9009_zc706
#make zynq_xcomm_adv7511_defconfig O=$OUT_DIR
#make zynq-zc706-adv7511-adrv9009.dtb O=$OUT_DIR

#git checkout ${BRANCH}
OUT_DIR=../out_${BRANCH}
make ${BRANCH}_defconfig O=${OUT_DIR}
if [ ${BRANCH} != "fhk_a10ad9371" ]; then
    #make -j5 UIMAGE_LOADADDR=0x8000 uImage O=${OUT_DIR} KCFLAGS=-DDEBUG
    make -j5 UIMAGE_LOADADDR=0x8000 uImage O=${OUT_DIR}
    make ${BRANCH}.dtb O=${OUT_DIR}
else
    #make all -j5 O=${OUT_DIR} KCFLAGS=-DDEBUG
    make -j5 O=${OUT_DIR}
fi

export ARCH=
export CROSS_COMPILE=
#arm_rm
