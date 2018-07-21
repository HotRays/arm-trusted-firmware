#ifndef __SPL_HEAD_H__
#define __SPL_HEAD_H__

struct spl_head {
	uint32_t  b_inst;
	uint32_t  magic[2];
	uint32_t  checksum;
	uint32_t  len;
	uint8_t   spl_sign[4]; /* the 4th byte is SPL version */
	uint32_t  fel_script_addr;
	uint32_t  fel_uenv_len;
	uint32_t  off_dt_name;
	uint32_t  res1;
	uint32_t  boot_media;
	uint32_t  string_pool[13];
};

#define SPL_MAGIC_eGON	0x4e4f4765 /* "eGON" */
#define SPL_MAGIC_BT0	0x3054422e /* ".BT0" */
#define SPL_MAGIC_FEL	0x4c45462e /* ".FEL" */

#define SPL_SIGN	"SPL" /* only the first 3 bytes */

#define SPL_VER_DTNAME	2

#endif
