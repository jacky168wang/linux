/*
# si5338_cfg
#
# Script to program Silicon Labs SI5338 Clock chip on Intel/Altera
# Arria 10 SoC DevKit
# by  David Koltak  Intel PSG  05/18/2016
#
#!/usr/bin/perl

if ($#ARGV != 1)
{ die "USAGE: $0 {i2c_dev_addr} {reg_map_file}\n"; }

$I2C_BUS_NUM = 0;
$I2C_SI5228_ADDR = $ARGV[0];
$I2C_REG_MAP = $ARGV[1];

#
# READ DATA ARRAY FROM REGISTER MAP FILE
#

open(_INFILE_, "<$I2C_REG_MAP") || die "ERROR: Unable to open '$I2C_REG_MAP'\n";
$SI5338_CFG_DATA = "";
$data_found = 0;
while ($line = <_INFILE_>)
{
  if ($data_found)
  {
    $line =~ s/\s+//g;
    $SI5338_CFG_DATA .= "$line\n";
    if ($line =~ /};/)
    { last; }
  }
  elsif ($line =~ /Reg_Data\s+const\s+code\s+Reg_Store\[NUM_REGS_MAX\]\s+=\s+{/)
  { $data_found = 1; }
}

close(_INFILE_);

#
# SCRIPT TO PROGRAM SI5338 PART
#

print "INFO: Disabling si5338 outputs\n";

write_reg(230, 0x10, 0x10);

print "INFO: Pausing si5338 LOL\n";

write_reg(241, 0x80, 0x80);

print "INFO: Writing register map to si5338\n";

@cfg_data = split(/\n/, $SI5338_CFG_DATA);
$idx = 0;

write_reg(255, 0x00, 0xFF);

foreach $cfg_set (@cfg_data)
{
  if ($cfg_set eq "")
  { next; }
  
  $idx++;
  
  if ($cfg_set =~ /{\s*(\d\w*)\s*,\s*(\d\w*)\s*,\s*(\d\w*)\s*}/)
  {
    $reg_waddr = $1;
    $reg_wdata = $2;
    $reg_wmask = $3;
    
    if ($reg_waddr =~ /^0/) { $reg_waddr = oct($reg_waddr); }
    if ($reg_wdata =~ /^0/) { $reg_wdata = oct($reg_wdata); }
    if ($reg_wmask =~ /^0/) { $reg_wmask = oct($reg_wmask); }
    
    write_reg($reg_waddr, $reg_wdata, $reg_wmask);
  }
  else { die "ERROR: Invalid config data line at index $idx\n"; }
}

print "INFO: Validating si5338 input clock status\n";

$rtn = read_reg(218);
while (($rtn & 0x04) != 0)
{ $rtn = read_reg(218); }

print "INFO: Configuring si5338 PLL for locking\n";

write_reg(49, 0x00, 0x80);

print "INFO: Initiating si5338 PLL lock sequence\n";

write_reg(246, 0x02, 0x02);
sleep(1); # NOTE: Only need 25 ms, but 1 second will work

print "INFO: Restarting si5338 LOL\n";

write_reg(241, 0x65, 0xFF);

print "INFO: Confirming si5338 PLL locked status\n";

$rtn = read_reg(218);
while (($rtn & 0x15) != 0)
{ $rtn = read_reg(218); }

print "INFO: Copying si5338 FCAL values to active reg\n";

$rtn = read_reg(237);
write_reg(47, $rtn, 0x03);

$rtn = read_reg(236);
write_reg(46, $rtn, 0xFF);

$rtn = read_reg(235);
write_reg(45, $rtn, 0xFF);

write_reg(47, 0x14, 0xFC);

print "INFO: Setting si5338 PLL to use FCAL values\n";

write_reg(49, 0x80, 0x80);

print "INFO: Enabling si5338 outputs\n";

write_reg(230, 0x00, 0x10);

print " * * * FINISHED * * *\n";
exit;            

#
# HELPER FUNCTIONS
#

sub read_reg
{
  my $addr = shift;
  my $data;
  my $cmd = "i2cget -y $I2C_BUS_NUM $I2C_SI5228_ADDR $addr";
  
  print "<-- $cmd\n";
  $data = `$cmd`;
  
  if (!($data =~ /^0x\w\w/))
  { die "ERROR: Bad return value '$data' from '$cmd'\n"; }
  
  return oct($data);
}

sub write_reg
{
  my $addr = shift;
  my $data = shift;
  my $mask = shift;
  my $cmd = "i2cset -y $I2C_BUS_NUM $I2C_SI5228_ADDR $addr $data";
  
  if ($mask == 0)
  { return 0; }
  elsif ($mask != 0xFF)
  {
    $data = ($data & $mask) | (read_reg($addr) & ~$mask);
    $cmd = "i2cset -y $I2C_BUS_NUM $I2C_SI5228_ADDR $addr $data";
  }
  
  print "--> $cmd\n";
  $data = system($cmd);
  
  if ($data)
  { die "ERROR: Bad return value '$data' from '$cmd'\n"; }
  
  return 0;
}
*/

/* NOTICE: on A10DK board, SW1.I2C_Flag should be LOW */
//#define DEBUG 1
#include <linux/bsearch.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/io.h>
#include <linux/unistd.h>
#include <linux/device.h>

#define PLL_POLLING_TMOUT 100

#define REG5338_PAGE			255
#define REG5338_PAGE_MASK		1
#define REG5338_DEV_CONFIG2		2
#define REG5338_DEV_CONFIG2_MASK	0x3f
#define REG5338_DEV_CONFIG2_VAL		38 /* last 2 digits of part number */
#define AWE_STATUS			0xdaff
#define AWE_FCAL_07_00		0xebff
#define AWE_FCAL_15_08		0xecff
#define AWE_FCAL_17_16		0xed03
#define AWE_SOFT_RESET		0xf602
#define NUM_REGS_MAX 350


/*
 * This array is used to determine if a register is writable. The mask is
 * not used in this driver. The data is in format of 0xAAAMM where AAA is
 * address, MM is bit mask. 1 means the corresponding bit is writable.
 * Created from SiLabs ClockBuilder output.
 * Note: Register 226, 230, 241, 246, 255 are not included in header file
 *	 from ClockBuilder v2.7 or later. Manually added here.
 */
static const u32 register_masks[] = {
	0x61d, 0x1b80, 0x1cff, 0x1dff, 0x1eff, 0x1fff, 0x20ff, 0x21ff,
	0x22ff, 0x23ff, 0x241f, 0x251f, 0x261f, 0x271f, 0x28ff, 0x297f,
	0x2a3f, 0x2bff, 0x2dff, 0x2eff, 0x2f3f, 0x30ff, 0x31ff, 0x32ff, 0x33ff,
	0x34ff, 0x35ff, 0x36ff, 0x37ff, 0x38ff, 0x39ff, 0x3aff, 0x3bff,
	0x3cff, 0x3dff, 0x3e3f, 0x3fff, 0x40ff, 0x41ff, 0x42ff, 0x43ff,
	0x44ff, 0x45ff, 0x46ff, 0x47ff, 0x48ff, 0x493f, 0x4aff, 0x4bff,
	0x4cff, 0x4dff, 0x4eff, 0x4fff, 0x50ff, 0x51ff, 0x52ff, 0x53ff,
	0x543f, 0x55ff, 0x56ff, 0x57ff, 0x58ff, 0x59ff, 0x5aff, 0x5bff,
	0x5cff, 0x5dff, 0x5eff, 0x5f3f, 0x61ff, 0x62ff, 0x63ff, 0x64ff,
	0x65ff, 0x66ff, 0x67ff, 0x68ff, 0x69ff, 0x6abf, 0x6bff, 0x6cff,
	0x6dff, 0x6eff, 0x6fff, 0x70ff, 0x71ff, 0x72ff, 0x73ff, 0x74ff,
	0x75ff, 0x76ff, 0x77ff, 0x78ff, 0x79ff, 0x7aff, 0x7bff, 0x7cff,
	0x7dff, 0x7eff, 0x7fff, 0x80ff, 0x810f, 0x820f, 0x83ff, 0x84ff,
	0x85ff, 0x86ff, 0x87ff, 0x88ff, 0x89ff, 0x8aff, 0x8bff, 0x8cff,
	0x8dff, 0x8eff, 0x8fff, 0x90ff, 0x98ff, 0x99ff, 0x9aff, 0x9bff,
	0x9cff, 0x9dff, 0x9e0f, 0x9f0f, 0xa0ff, 0xa1ff, 0xa2ff, 0xa3ff,
	0xa4ff, 0xa5ff, 0xa6ff, 0xa7ff, 0xa8ff, 0xa9ff, 0xaaff, 0xabff,
	0xacff, 0xadff, 0xaeff, 0xafff, 0xb0ff, 0xb1ff, 0xb2ff, 0xb3ff,
	0xb4ff, 0xb50f, 0xb6ff, 0xb7ff, 0xb8ff, 0xb9ff, 0xbaff, 0xbbff,
	0xbcff, 0xbdff, 0xbeff, 0xbfff, 0xc0ff, 0xc1ff, 0xc2ff, 0xc3ff,
	0xc4ff, 0xc5ff, 0xc6ff, 0xc7ff, 0xc8ff, 0xc9ff, 0xcaff, 0xcb0f,
	0xccff, 0xcdff, 0xceff, 0xcfff, 0xd0ff, 0xd1ff, 0xd2ff, 0xd3ff,
	0xd4ff, 0xd5ff, 0xd6ff, 0xd7ff, 0xd8ff, 0xd9ff, 0xe204, 0xe6ff,
	0xf1ff, 0xf202, 0xf6ff, 0xffff, 0x11fff,
	0x120ff, 0x121ff, 0x122ff, 0x123ff, 0x124ff, 0x125ff, 0x126ff, 0x127ff,
	0x128ff, 0x129ff, 0x12aff, 0x12b0f, 0x12fff, 0x130ff, 0x131ff, 0x132ff,
	0x133ff, 0x134ff, 0x135ff, 0x136ff, 0x137ff, 0x138ff, 0x139ff, 0x13aff,
	0x13b0f, 0x13fff, 0x140ff, 0x141ff, 0x142ff, 0x143ff, 0x144ff, 0x145ff,
	0x146ff, 0x147ff, 0x148ff, 0x149ff, 0x14aff, 0x14b0f, 0x14fff, 0x150ff,
	0x151ff, 0x152ff, 0x153ff, 0x154ff, 0x155ff, 0x156ff, 0x157ff, 0x158ff,
	0x159ff, 0x15aff, 0x15b0f
};

static int si5338_find_msk(const void *key, const void *elt)
{
	const u32 *reg = key;
	const u32 *msk = elt;

	if (*reg > *msk >> 8)
		return 1;
	if (*reg < *msk >> 8)
		return -1;

	return 0;
}

static bool si5338_regmap_is_writeable(struct device *dev, unsigned int reg)
{
	return bsearch(&reg, register_masks, ARRAY_SIZE(register_masks),
		       sizeof(u32), si5338_find_msk) != NULL;
}

static bool si5338_regmap_is_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case (AWE_STATUS >> 8):
	case (AWE_SOFT_RESET >> 8):
	case (AWE_FCAL_07_00 >> 8):
	case (AWE_FCAL_15_08 >> 8):
	case (AWE_FCAL_17_16 >> 8):
		return true;
	}

	return false;
}

/* reg, val, msk */
static const unsigned short si5338_regs[NUM_REGS_MAX][3] = {
{  0,0x00,0x00},
{  1,0x00,0x00},
{  2,0x00,0x00},
{  3,0x00,0x00},
{  4,0x00,0x00},
{  5,0x00,0x00},
{  6,0x08,0x1D},
{  7,0x00,0x00},
{  8,0x70,0x00},
{  9,0x0F,0x00},
{ 10,0x00,0x00},
{ 11,0x00,0x00},
{ 12,0x00,0x00},
{ 13,0x00,0x00},
{ 14,0x00,0x00},
{ 15,0x00,0x00},
{ 16,0x00,0x00},
{ 17,0x00,0x00},
{ 18,0x00,0x00},
{ 19,0x00,0x00},
{ 20,0x00,0x00},
{ 21,0x00,0x00},
{ 22,0x00,0x00},
{ 23,0x00,0x00},
{ 24,0x00,0x00},
{ 25,0x00,0x00},
{ 26,0x00,0x00},
{ 27,0x70,0x80},
{ 28,0x16,0xFF},
{ 29,0x90,0xFF},
{ 30,0xB0,0xFF},
{ 31,0xC0,0xFF},
{ 32,0xC0,0xFF},
{ 33,0xC0,0xFF},
{ 34,0xC0,0xFF},
{ 35,0xAA,0xFF},
{ 36,0x06,0x1F},
{ 37,0x06,0x1F},
{ 38,0x06,0x1F},
{ 39,0x06,0x1F},
{ 40,0x84,0xFF},
{ 41,0x10,0x7F},
{ 42,0x24,0x3F},
{ 43,0x00,0x00},
{ 44,0x00,0x00},
{ 45,0x00,0xFF},
{ 46,0x00,0xFF},
{ 47,0x14,0x3F},
{ 48,0x36,0xFF},
{ 49,0x00,0xFF},
{ 50,0xC3,0xFF},
{ 51,0x07,0xFF},
{ 52,0x10,0xFF},
{ 53,0x50,0xFF},
{ 54,0x08,0xFF},
{ 55,0x00,0xFF},
{ 56,0x00,0xFF},
{ 57,0x00,0xFF},
{ 58,0x00,0xFF},
{ 59,0x08,0xFF},
{ 60,0x00,0xFF},
{ 61,0x00,0xFF},
{ 62,0x00,0x3F},
{ 63,0x10,0xFF},
{ 64,0xC6,0xFF},
{ 65,0x02,0xFF},
{ 66,0x80,0xFF},
{ 67,0x00,0xFF},
{ 68,0x00,0xFF},
{ 69,0x00,0xFF},
{ 70,0x90,0xFF},
{ 71,0x00,0xFF},
{ 72,0x00,0xFF},
{ 73,0x00,0x3F},
{ 74,0x10,0xFF},
{ 75,0x00,0xFF},
{ 76,0x02,0xFF},
{ 77,0x00,0xFF},
{ 78,0x00,0xFF},
{ 79,0x00,0xFF},
{ 80,0x00,0xFF},
{ 81,0x01,0xFF},
{ 82,0x00,0xFF},
{ 83,0x00,0xFF},
{ 84,0x00,0x3F},
{ 85,0x10,0xFF},
{ 86,0x50,0xFF},
{ 87,0x08,0xFF},
{ 88,0x00,0xFF},
{ 89,0x00,0xFF},
{ 90,0x00,0xFF},
{ 91,0x00,0xFF},
{ 92,0x08,0xFF},
{ 93,0x00,0xFF},
{ 94,0x00,0xFF},
{ 95,0x00,0x3F},
{ 96,0x10,0x00},
{ 97,0x90,0xFF},
{ 98,0x31,0xFF},
{ 99,0x00,0xFF},
{100,0x00,0xFF},
{101,0x00,0xFF},
{102,0x00,0xFF},
{103,0x08,0xFF},
{104,0x00,0xFF},
{105,0x00,0xFF},
{106,0x80,0xBF},
{107,0x00,0xFF},
{108,0x00,0xFF},
{109,0x00,0xFF},
{110,0x80,0xFF},
{111,0x00,0xFF},
{112,0x00,0xFF},
{113,0x00,0xFF},
{114,0x80,0xFF},
{115,0x00,0xFF},
{116,0x80,0xFF},
{117,0x00,0xFF},
{118,0x80,0xFF},
{119,0x00,0xFF},
{120,0x00,0xFF},
{121,0x00,0xFF},
{122,0x80,0xFF},
{123,0x00,0xFF},
{124,0x00,0xFF},
{125,0x00,0xFF},
{126,0x00,0xFF},
{127,0x00,0xFF},
{128,0x00,0xFF},
{129,0x00,0x0F},
{130,0x00,0x0F},
{131,0x00,0xFF},
{132,0x00,0xFF},
{133,0x00,0xFF},
{134,0x00,0xFF},
{135,0x00,0xFF},
{136,0x00,0xFF},
{137,0x00,0xFF},
{138,0x00,0xFF},
{139,0x00,0xFF},
{140,0x00,0xFF},
{141,0x00,0xFF},
{142,0x00,0xFF},
{143,0x00,0xFF},
{144,0x00,0xFF},
{145,0x00,0x00},
{146,0xFF,0x00},
{147,0x00,0x00},
{148,0x00,0x00},
{149,0x00,0x00},
{150,0x00,0x00},
{151,0x00,0x00},
{152,0x00,0xFF},
{153,0x00,0xFF},
{154,0x00,0xFF},
{155,0x00,0xFF},
{156,0x00,0xFF},
{157,0x00,0xFF},
{158,0x00,0x0F},
{159,0x00,0x0F},
{160,0x00,0xFF},
{161,0x00,0xFF},
{162,0x00,0xFF},
{163,0x00,0xFF},
{164,0x00,0xFF},
{165,0x00,0xFF},
{166,0x00,0xFF},
{167,0x00,0xFF},
{168,0x00,0xFF},
{169,0x00,0xFF},
{170,0x00,0xFF},
{171,0x00,0xFF},
{172,0x00,0xFF},
{173,0x00,0xFF},
{174,0x00,0xFF},
{175,0x00,0xFF},
{176,0x00,0xFF},
{177,0x00,0xFF},
{178,0x00,0xFF},
{179,0x00,0xFF},
{180,0x00,0xFF},
{181,0x00,0x0F},
{182,0x00,0xFF},
{183,0x00,0xFF},
{184,0x00,0xFF},
{185,0x00,0xFF},
{186,0x00,0xFF},
{187,0x00,0xFF},
{188,0x00,0xFF},
{189,0x00,0xFF},
{190,0x00,0xFF},
{191,0x00,0xFF},
{192,0x00,0xFF},
{193,0x00,0xFF},
{194,0x00,0xFF},
{195,0x00,0xFF},
{196,0x00,0xFF},
{197,0x00,0xFF},
{198,0x00,0xFF},
{199,0x00,0xFF},
{200,0x00,0xFF},
{201,0x00,0xFF},
{202,0x00,0xFF},
{203,0x00,0x0F},
{204,0x00,0xFF},
{205,0x00,0xFF},
{206,0x00,0xFF},
{207,0x00,0xFF},
{208,0x00,0xFF},
{209,0x00,0xFF},
{210,0x00,0xFF},
{211,0x00,0xFF},
{212,0x00,0xFF},
{213,0x00,0xFF},
{214,0x00,0xFF},
{215,0x00,0xFF},
{216,0x00,0xFF},
{217,0x00,0xFF},
{218,0x00,0x00},
{219,0x00,0x00},
{220,0x00,0x00},
{221,0x0D,0x00},
{222,0x00,0x00},
{223,0x00,0x00},
{224,0xF4,0x00},
{225,0xF0,0x00},
{226,0x00,0x00},
{227,0x00,0x00},
{228,0x00,0x00},
{229,0x00,0x00},
{231,0x00,0x00},
{232,0x00,0x00},
{233,0x00,0x00},
{234,0x00,0x00},
{235,0x00,0x00},
{236,0x00,0x00},
{237,0x00,0x00},
{238,0x14,0x00},
{239,0x00,0x00},
{240,0x00,0x00},
{242,0x02,0x02},
{243,0xF0,0x00},
{244,0x00,0x00},
{245,0x00,0x00},
{247,0x00,0x00},
{248,0x00,0x00},
{249,0xA8,0x00},
{250,0x00,0x00},
{251,0x84,0x00},
{252,0x00,0x00},
{253,0x00,0x00},
{254,0x00,0x00},
{255,   1,0xFF}, // set page bit to 1 
{  0+256,0x00,0x00},
{  1+256,0x00,0x00},
{  2+256,0x00,0x00},
{  3+256,0x00,0x00},
{  4+256,0x00,0x00},
{  5+256,0x00,0x00},
{  6+256,0x00,0x00},
{  7+256,0x00,0x00},
{  8+256,0x00,0x00},
{  9+256,0x00,0x00},
{ 10+256,0x00,0x00},
{ 11+256,0x00,0x00},
{ 12+256,0x00,0x00},
{ 13+256,0x00,0x00},
{ 14+256,0x00,0x00},
{ 15+256,0x00,0x00},
{ 16+256,0x00,0x00},
{ 17+256,0x01,0x00},
{ 18+256,0x00,0x00},
{ 19+256,0x00,0x00},
{ 20+256,0x90,0x00},
{ 21+256,0x31,0x00},
{ 22+256,0x00,0x00},
{ 23+256,0x00,0x00},
{ 24+256,0x01,0x00},
{ 25+256,0x00,0x00},
{ 26+256,0x00,0x00},
{ 27+256,0x00,0x00},
{ 28+256,0x00,0x00},
{ 29+256,0x00,0x00},
{ 30+256,0x00,0x00},
{ 31+256,0x00,0xFF},
{ 32+256,0x00,0xFF},
{ 33+256,0x01,0xFF},
{ 34+256,0x00,0xFF},
{ 35+256,0x00,0xFF},
{ 36+256,0x90,0xFF},
{ 37+256,0x31,0xFF},
{ 38+256,0x00,0xFF},
{ 39+256,0x00,0xFF},
{ 40+256,0x01,0xFF},
{ 41+256,0x00,0xFF},
{ 42+256,0x00,0xFF},
{ 43+256,0x00,0x0F},
{ 44+256,0x00,0x00},
{ 45+256,0x00,0x00},
{ 46+256,0x00,0x00},
{ 47+256,0x00,0xFF},
{ 48+256,0x00,0xFF},
{ 49+256,0x01,0xFF},
{ 50+256,0x00,0xFF},
{ 51+256,0x00,0xFF},
{ 52+256,0x90,0xFF},
{ 53+256,0x31,0xFF},
{ 54+256,0x00,0xFF},
{ 55+256,0x00,0xFF},
{ 56+256,0x01,0xFF},
{ 57+256,0x00,0xFF},
{ 58+256,0x00,0xFF},
{ 59+256,0x00,0x0F},
{ 60+256,0x00,0x00},
{ 61+256,0x00,0x00},
{ 62+256,0x00,0x00},
{ 63+256,0x00,0xFF},
{ 64+256,0x00,0xFF},
{ 65+256,0x01,0xFF},
{ 66+256,0x00,0xFF},
{ 67+256,0x00,0xFF},
{ 68+256,0x90,0xFF},
{ 69+256,0x31,0xFF},
{ 70+256,0x00,0xFF},
{ 71+256,0x00,0xFF},
{ 72+256,0x01,0xFF},
{ 73+256,0x00,0xFF},
{ 74+256,0x00,0xFF},
{ 75+256,0x00,0x0F},
{ 76+256,0x00,0x00},
{ 77+256,0x00,0x00},
{ 78+256,0x00,0x00},
{ 79+256,0x00,0xFF},
{ 80+256,0x00,0xFF},
{ 81+256,0x00,0xFF},
{ 82+256,0x00,0xFF},
{ 83+256,0x00,0xFF},
{ 84+256,0x90,0xFF},
{ 85+256,0x31,0xFF},
{ 86+256,0x00,0xFF},
{ 87+256,0x00,0xFF},
{ 88+256,0x01,0xFF},
{ 89+256,0x00,0xFF},
{ 90+256,0x00,0xFF},
{ 91+256,0x00,0x0F},
{ 92+256,0x00,0x00},
{ 93+256,0x00,0x00},
{ 94+256,0x00,0x00},
{255,   0,0xFF} // set page bit to 0
};

static const struct regmap_range_cfg si5338_regmap_range[] = {
	{
		.selector_reg = REG5338_PAGE,		/* 255 */
		.selector_mask  = REG5338_PAGE_MASK,	/* 1 */
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 256,
		.range_min = 0,
		.range_max = 347,
	},
};

static const struct regmap_config si5338_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.max_register = 347,
	.ranges = si5338_regmap_range,
	.num_ranges = ARRAY_SIZE(si5338_regmap_range),
	.writeable_reg = si5338_regmap_is_writeable,
	.volatile_reg = si5338_regmap_is_volatile,
};

/*
 * SI5338 register access
 */

struct si5338simple_t {
	struct i2c_client *client;
	struct regmap *regmap;
};

static inline int si5338simple_reg_read(struct si5338simple_t *drvdata, u16 reg, u8 *val)
{
    int ret;
	unsigned int tmp;

	ret = regmap_read(drvdata->regmap, reg, &tmp);
	if (ret) {
		dev_err(&drvdata->client->dev, "regmap_read failed: [0x%x] -> 0x%x\n",
			reg, tmp);
		return -ENODEV;
	}
	dev_dbg(&drvdata->client->dev, "i2cget -y 0 0x71 %d -> %d\n", reg, tmp);
	*val = (u8)tmp;
	return 0;
}

static inline int si5338simple_reg_write(struct si5338simple_t *drvdata, u8 val, u16 reg, u8 msk)
{
	int ret;
	unsigned int tmp;

	if (msk == 0x00) return 0;

#if 0
	regmap_update_bits(drvdata->regmap, reg, msk, val);
#else
	if (msk == 0xff) tmp = val;
	else {
		ret = regmap_read(drvdata->regmap, reg, &tmp);
		if (ret) {
			dev_err(&drvdata->client->dev, "regmap_read failed: [0x%x] -> 0x%x\n",
				reg, tmp);
			return -ENODEV;
		}
		tmp &= ~msk;
		tmp |= val & msk;
	}

	ret = regmap_write(drvdata->regmap, reg, tmp);
	if (ret < 0) {
		dev_err(&drvdata->client->dev, "regmap_write failed: [0x%x] <- 0x%x\n",
			reg, tmp);
		return -ENODEV;
	}
#endif
	dev_dbg(&drvdata->client->dev, "i2cset -y 0 0x71 %d %d\n",
		reg, tmp);
	return 0;
}

/*
 * Si5351 i2c probe and device tree parsing
 */

#ifdef CONFIG_OF
static int si5338simple_dt_parse(struct i2c_client *client)
{
	struct device_node *np = client->dev.of_node;

	if (np == NULL)
		return 0;

	return 0;
}

static const struct of_device_id si5338simple_dt_ids[] = {
	{ .compatible = "silabs,si5338" },
	{ }
};
MODULE_DEVICE_TABLE(of, si5338simple_dt_ids);
#else
static int si5338simple_dt_parse(struct i2c_client *client)
{
	return 0;
}
#endif /* CONFIG_OF */

static int si5338simple_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret, i;
	u8 val;
	struct si5338simple_t *drvdata;

	dev_info(&client->dev, "%s: enter", __func__);

	drvdata = devm_kzalloc(&client->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	si5338simple_dt_parse(client);

	i2c_set_clientdata(client, drvdata);
	drvdata->client = client;

	/* Register regmap */
	drvdata->regmap = devm_regmap_init_i2c(client, &si5338_regmap_config);
	if (IS_ERR(drvdata->regmap)) {
		dev_err(&client->dev, "devm_regmap_init_i2c failed 0x%x\n", drvdata->regmap);
		return PTR_ERR(drvdata->regmap);
	}

	/* Check if si5338 exists */ 
	ret = si5338simple_reg_read(drvdata, REG5338_DEV_CONFIG2, &val);
	if (ret < 0) return -ENODEV;
	if ((val & REG5338_DEV_CONFIG2_MASK) != REG5338_DEV_CONFIG2_VAL) {
		dev_err(&client->dev, "[%d] -> 0x%x but expected 0x%x\n",
			REG5338_DEV_CONFIG2, val, REG5338_DEV_CONFIG2_VAL);
		return -ENODEV;
	}
	ret = si5338simple_reg_read(drvdata, 0, &val);
	if (ret < 0) return -ENODEV;
	dev_info(&client->dev, "si5338 Rev.%d detected\n", val & 0x7);

	dev_dbg(&client->dev, "Disabling si5338 outputs\n");
	ret = si5338simple_reg_write(drvdata, 0x10, 230, 0x10);
	if (ret < 0) return -ENODEV;

	dev_dbg(&client->dev, "Pausing LOL\n");
	ret = si5338simple_reg_write(drvdata, 0x80, 241, 0x80);
	if (ret < 0) return -ENODEV;
	ret = si5338simple_reg_write(drvdata, 0x00, 255, 0xff);
	if (ret < 0) return -ENODEV;

	dev_dbg(&client->dev, "Writing registers table produced by CLOCKBUILDER DESKTOP\n");
	for (i = 0; i < ARRAY_SIZE(si5338_regs); i++) {
		ret = si5338simple_reg_write(drvdata, si5338_regs[i][1],
			si5338_regs[i][0], si5338_regs[i][2]);
		if (ret < 0) return -ENODEV;
	}

	dev_dbg(&client->dev, "Confirming PLL locked status\n");
	i = PLL_POLLING_TMOUT;
	do {
		msleep(1);
		ret = si5338simple_reg_read(drvdata, 218, &val);
	    if (ret < 0) return -ENODEV;
		if (!(val & 0x04)) break;
	} while (i--);
	if (0 == i) {
		dev_err(&client->dev, "Timeout(%d ms): polling [218] for 0x04 but 0x%x\n",
			PLL_POLLING_TMOUT, val); 
		return -ETIMEDOUT;
	}
	dev_info(&client->dev, "PLL locked without LOL\n");

	dev_dbg(&client->dev, "Configuring PLL for locking\n");
	ret = si5338simple_reg_write(drvdata, 0x00, 49, 0xFF);
	if (ret < 0) return -ENODEV;

	dev_dbg(&client->dev, "Initiating PLL lock sequence\n");
	ret = si5338simple_reg_write(drvdata,  0x02, 246, 0x02);
	if (ret < 0) return -ENODEV;

	msleep(25); /* NOTE: need 25 ms per datasheet */

	dev_dbg(&client->dev, "Restarting LOL\n");
	ret = si5338simple_reg_write(drvdata, 0x65, 241, 0xFF);
	if (ret < 0) return -ENODEV;

	dev_dbg(&client->dev, "Confirming PLL locked status\n");
	i = PLL_POLLING_TMOUT;
	do {
		msleep(1);
		ret = si5338simple_reg_read(drvdata, 218, &val);
	    if (ret < 0) return -ENODEV;
		if (!(val & 0x15)) break;
	} while (i--);
	if (0 == i) {
		dev_err(&client->dev, "Timeout(%d ms): polling [218] for 0x15 but 0x%x\n",
			PLL_POLLING_TMOUT, val); 
		return -ETIMEDOUT;
	}
	dev_info(&client->dev, "PLL locked with LOL\n");

	dev_dbg(&client->dev, "Copying FCAL values to active\n");
	ret = si5338simple_reg_read(drvdata, 237, &val);
	if (ret < 0) return -ENODEV;
	ret = si5338simple_reg_write(drvdata, val, 47,  0x03);
	if (ret < 0) return -ENODEV;
	ret = si5338simple_reg_read(drvdata, 236, &val);
	if (ret < 0) return -ENODEV;
	ret = si5338simple_reg_write(drvdata, val, 46, 0xFF);
	if (ret < 0) return -ENODEV;
	ret = si5338simple_reg_read(drvdata, 235, &val);
	if (ret < 0) return -ENODEV;
	ret = si5338simple_reg_write(drvdata, val, 45, 0xFF);
	if (ret < 0) return -ENODEV;
	ret = si5338simple_reg_write(drvdata, 0x14, 47, 0xFC);
	if (ret < 0) return -ENODEV;
	dev_dbg(&client->dev, "Setting PLL to use FCAL values\n");
	ret = si5338simple_reg_write(drvdata , 0x80, 49, 0x80);
	if (ret < 0) return -ENODEV;
	dev_dbg(&client->dev, "Enabling outputs\n");
	ret = si5338simple_reg_write(drvdata, 0x00, 230, 0x10);
	if (ret < 0) return -ENODEV;

	dev_info(&client->dev, "%s: succeed", __func__);
	return 0;
}

static int si5338simple_remove(struct i2c_client *client)
{
	struct si5338simple_t *drvdata = i2c_get_clientdata(client);

    //misc_deregister(&drvdata->misc_dev);
	//kfree(drvdata);

	dev_info(&client->dev, "Removed\n");
	return 0;
}

static const struct i2c_device_id si5338_hw_ids[] = {
	{ "si5338", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, si5338_hw_ids);

static struct i2c_driver si5338simple_driver = {
	.driver = {
		.name	= "si5338simple",
		.owner	= THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(si5338simple_dt_ids),
#endif
	},
	.probe		= si5338simple_probe,
	.remove		= si5338simple_remove,
	.id_table	= si5338_hw_ids,
};
module_i2c_driver(si5338simple_driver);

MODULE_AUTHOR("David Koltak, Intel PSG");
MODULE_DESCRIPTION("si5338simple based on 'si5338_cfg' from Altera");
MODULE_LICENSE("GPL v2");
