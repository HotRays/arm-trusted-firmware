#include <arch.h>
#include <assert.h>
#include <cci.h>
#include <debug.h>
#include <mmio.h>
#include <stdint.h>
#include "sunxi_i2c.h"
#include <delay_timer.h>

#define TWI_CONTROL_OFFSET             0x400
#define SUNXI_I2C_CONTROLLER             3

struct i2c_adapter adapter_axp806;
struct i2c_adapter *adapt_axp806=&adapter_axp806;

struct sunxi_twi_reg
{
    volatile unsigned int addr;        /* slave address     */
    volatile unsigned int xaddr;       /* extend address    */
    volatile unsigned int data;        /* data              */
    volatile unsigned int ctl;         /* control           */
    volatile unsigned int status;      /* status            */
    volatile unsigned int clk;         /* clock             */
    volatile unsigned int srst;        /* soft reset        */
    volatile unsigned int eft;         /* enhanced future   */
    volatile unsigned int lcr;         /* line control      */
    volatile unsigned int dvfs;        /* dvfs control      */
};

//#define SUNXI_I2C_DEBUG
#if defined(SUNXI_I2C_DEBUG)
#define I2C_DBG(msg...)                                                        \
	do {                                                                   \
		{                                                              \
			printf("[I2C-DBG] %s,line:%d:    ", __func__, __LINE__); \
			printf(msg);                                           \
		}                                                              \
	} while (0)
#else
#define I2C_DBG(fmt, arg...)
#endif /*endif defined(_SUNXI_I2C_DEBUG) */

#define I2C_ERR(fmt, arg...)	printf("[I2C-ERROR]:%s() %d "fmt, __func__, __LINE__)
#define I2C_WRN(fmt, arg...)	printf("[I2C-WRN]:%s() %d "fmt, __func__, __LINE__)

#define __usdelay udelay

#define readl(a) mmio_read_32((a))
#define writel(c,v) mmio_write_32((v),(c))

#define MAX_SUNXI_I2C_NUM 5
__attribute__((section(".data")))
static  struct sunxi_twi_reg *sunxi_i2c[MAX_SUNXI_I2C_NUM] = {NULL, NULL, NULL, NULL};

static inline void twi_soft_reset(int bus_num)
{
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];
	i2c->eft  = 0;
	i2c->srst = 1;
}

static inline void twi_set_start(int bus_num)
{
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];

	i2c->ctl  |= TWI_CTL_STA;
	i2c->ctl  &= ~TWI_CTL_INTFLG;
}

static inline u32 twi_get_start(int bus_num)
{
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];
	i2c->ctl  >>= 5;
	return i2c->ctl  & 1;
}

static inline void twi_clear_irq_flag(int bus_num)
{
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];
	/* start and stop bit should be 0 */
	i2c->ctl |= TWI_CTL_INTFLG;
	i2c->ctl &= ~(TWI_CTL_STA | TWI_CTL_STP);
}

static inline int twi_wait_irq_flag_clear(int bus_num, u32 time)
{
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];

	while ( (time--) && (!(i2c->ctl & TWI_CTL_INTFLG)) );

	if (time <= 0)
		return SUNXI_I2C_TOUT;

	return SUNXI_I2C_OK;
}

static inline void twi_enable_ack(int bus_num)
{
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];
	i2c->ctl |= TWI_CTL_ACK;
	i2c->ctl &= ~TWI_CTL_INTFLG;
}

static inline void twi_disable_ack(int bus_num)
{
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];
	i2c->ctl &= ~TWI_CTL_ACK;
	i2c->ctl &= ~TWI_CTL_INTFLG;
}

static inline void twi_set_stop(int bus_num)
{
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];
	i2c->ctl  |= TWI_CTL_STP;
	i2c->ctl  &= ~TWI_CTL_INTFLG;
}

static inline u32 twi_get_stop(int bus_num)
{
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];
	int reg_val = i2c->ctl;
	reg_val >>= 4;
	return reg_val & 1;
}

static void twi_enable_lcr(int bus_num)
{
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];

	i2c->lcr |= TWI_LCR_SCL_EN;
	i2c->lcr |= TWI_LCR_SDA_EN;
}

/* send 9 clock to release sda */
static int twi_send_clk_9pulse(int bus_num)
{
	int cycle = 10;

	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];
	twi_enable_lcr(bus_num);
	__usdelay(500);

	/* toggle I2C SCL and SDA until bus idle */
	while ((cycle > 0) && ((i2c->lcr & TWI_LCR_SDA_CTL) != TWI_LCR_SDA_CTL)) {
		/*control scl and sda output high level*/
		i2c->lcr |= TWI_LCR_SCL_CTL;
		i2c->lcr |= TWI_LCR_SDA_CTL;
		__usdelay(1000);
		/*control scl and sda output low level*/
		i2c->lcr &= ~TWI_LCR_SCL_CTL;
		i2c->lcr &= ~TWI_LCR_SDA_CTL;
		__usdelay(1000);
		cycle--;
	}

	if ((i2c->lcr & TWI_LCR_SDA_CTL) != TWI_LCR_SDA_CTL) {
		I2C_ERR("SDA is still Stuck Low, failed. \n");
		return SUNXI_I2C_FAIL;
	}

	i2c->lcr = 0x0;
	__usdelay(500);

	return SUNXI_I2C_OK;
}

static void i2c_set_clock(int bus_num, int speed)
{
	int timeout, clk_n, clk_m, i, pow_2_clk_n;
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];

	timeout = 0xff;
	twi_soft_reset(bus_num);

	while ((i2c->srst) && (timeout--));

	if ((i2c->lcr & TWI_LCR_NORM_STATUS) != TWI_LCR_NORM_STATUS ) {
		I2C_DBG("[i2c%d] bus is busy, lcr = %x\n", bus_num, i2c->lcr);
		twi_send_clk_9pulse(bus_num);
	}
	speed /= 1000; /*khz*/ 

	if (speed < 100)
		speed = 100;
	else if (speed > 400)
		speed = 400;
	/*Foscl=24000/(2^CLK_N*(CLK_M+1)*10)*/
	clk_n = (speed == 100) ? 1 : 0;
	pow_2_clk_n = 1;
	for (i = 0; i < clk_n; ++i)
		pow_2_clk_n *= 2;
	clk_m = 2400 / (pow_2_clk_n * speed) - 1;

	i2c->clk = (clk_m << 3) | clk_n;
	i2c->ctl |= TWI_CTL_BUSEN;
	i2c->eft = 0;

}

static int twi_send_slave_addr(int bus_num, u32 saddr,  u32 rw)
{
	u32  time = 0xff;
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];

	rw &= 1;
	i2c->data = ((saddr & 0xff) << 1) | rw;
	twi_clear_irq_flag(bus_num);

	if (twi_wait_irq_flag_clear(bus_num, time))
		return SUNXI_I2C_TOUT;

	if ((rw == I2C_WRITE) && (i2c->status != I2C_ADDRWRITE_ACK)) {
		return -I2C_ADDRWRITE_ACK;
	}
	else if ((rw == I2C_READ) && (i2c->status != I2C_ADDRREAD_ACK))
		return -I2C_ADDRREAD_ACK;

	return SUNXI_I2C_OK;
}

static int  twi_send_byte_addr(int bus_num, u32 byteaddr)
{
	int  time = 0xff;
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];

	i2c->data = byteaddr & 0xff;
	twi_clear_irq_flag(bus_num);

	if (twi_wait_irq_flag_clear(bus_num, time))
		return SUNXI_I2C_TOUT;

	if (i2c->status != I2C_DATAWRITE_ACK)
		return -I2C_DATAWRITE_ACK;

	return SUNXI_I2C_OK;
}


static int twi_send_addr(int bus_num, uint addr, int alen)
{
	int i, ret, addr_len;
	char  *slave_reg;

	if (alen >= 3)
		addr_len = 2;
	else if (alen <= 1)
		addr_len = 0;
	else
		addr_len = 1;

	slave_reg = (char *)&addr;

	for (i = addr_len; i >= 0; i--) {
		ret = twi_send_byte_addr(bus_num, slave_reg[i] & 0xff);

		if (ret != SUNXI_I2C_OK)
			goto twi_send_addr_err;
	}

twi_send_addr_err:
	return ret;

}

static int twi_get_data(int bus_num, u8 *data_addr, u32 data_count)
{
	int  time = 0xff;
	u32  i;
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];

	if (data_count == 1) {
		/*no need ack  */
		twi_clear_irq_flag(bus_num);

		if (twi_wait_irq_flag_clear(bus_num, time))
			return SUNXI_I2C_TOUT;

		*data_addr = i2c->data;

		if ( i2c->status != I2C_DATAREAD_NACK)
			return -I2C_DATAREAD_NACK;
	} else {
		for (i = 0; i < data_count - 1; i++) {
			/*need ack  */
			twi_enable_ack(bus_num);
			twi_clear_irq_flag(bus_num);

			if (twi_wait_irq_flag_clear(bus_num, time))
				return SUNXI_I2C_TOUT;

			data_addr[i] = i2c->data;

			while ( (time--) && (i2c->status != I2C_DATAREAD_ACK) );

			if (time <= 0)
				return SUNXI_I2C_TOUT;
		}

		/* received the last byte  */
		twi_disable_ack(bus_num);
		twi_clear_irq_flag(bus_num);

		if (twi_wait_irq_flag_clear(bus_num, time))
			return SUNXI_I2C_TOUT;

		data_addr[data_count - 1] = i2c->data;

		while ( (time--) && (i2c->status != I2C_DATAREAD_NACK) );

		if (time <= 0)
			return SUNXI_I2C_TOUT;
	}

	return SUNXI_I2C_OK;
}

static int twi_send_data(int bus_num, u8  *data_addr, u32 data_count)
{
	int  time = 0xff;
	u32  i;
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];

	for (i = 0; i < data_count; i++) {
		i2c->data = data_addr[i];
		twi_clear_irq_flag(bus_num);

		if (twi_wait_irq_flag_clear(bus_num, time))
			return SUNXI_I2C_TOUT;

		time = 0xff;

		while ( (time--) && (i2c->status != I2C_DATAWRITE_ACK) );

		if (time <= 0)
			return SUNXI_I2C_TOUT;
	}

	return SUNXI_I2C_OK;
}

static int twi_restart(int bus_num)
{
	u32 timeout = 0xff;

	twi_set_start(bus_num);
	twi_clear_irq_flag(bus_num);
	if (twi_wait_irq_flag_clear(bus_num,timeout)) {
		I2C_ERR("Restart can't sendout!\n");
		return SUNXI_I2C_FAIL;
	}

	return SUNXI_I2C_OK;
}

static int twi_start(int bus_num)
{
	u32 timeout = 0xff;
	twi_soft_reset(bus_num);
	twi_set_start(bus_num);

	if (twi_wait_irq_flag_clear(bus_num,timeout)) {
		I2C_ERR("START can't sendout!\n");
		return SUNXI_I2C_FAIL;
	}

	return SUNXI_I2C_OK;
}

static int twi_stop(int bus_num)
{
	int  time = 0xff;
	struct sunxi_twi_reg * i2c = sunxi_i2c[bus_num];

	i2c->ctl |= (0x01 << 4);
	i2c->ctl |= (0x01 << 3);

	twi_set_stop(bus_num);
	twi_clear_irq_flag(bus_num);

	while (( 1 == twi_get_stop(bus_num)) && (--time));

	if (time == 0) {
		I2C_ERR("STOP can't sendout!\n");
		return SUNXI_I2C_TFAIL;
	}

	time = 0xff;

	while ((TWI_STAT_IDLE != i2c->status) && (--time));

	if (time <= 0) {
		I2C_ERR("i2c state isn't idle(0xf8)\n");
		return SUNXI_I2C_TFAIL;
	}

	return SUNXI_I2C_OK;
}

static int sunxi_i2c_read(struct i2c_adapter *adap, uint8_t chip,
				uint32_t addr, int alen, uint8_t *buffer, int len)
{
	int  ret;

	ret = twi_start(adap->hwadapnr);
	if (ret) {
		I2C_ERR("error\n");
		goto i2c_read_err_occur;
	}

	ret = twi_send_slave_addr(adap->hwadapnr, chip, I2C_WRITE);
	if (ret){
		I2C_DBG("[%x] error %d\n", chip, ret);
		goto i2c_read_err_occur;
	}

	ret = twi_send_addr(adap->hwadapnr, addr, alen);
	if (ret){
		goto i2c_read_err_occur;
	}

	ret = twi_restart(adap->hwadapnr);
	if (ret) {
		goto i2c_read_err_occur;
	}

	ret = twi_send_slave_addr(adap->hwadapnr, chip, I2C_READ);
	if (ret) {
		goto i2c_read_err_occur;
	}

	ret = twi_get_data(adap->hwadapnr, buffer, len);
	if (ret) {
		goto i2c_read_err_occur;
	}

i2c_read_err_occur:
	twi_stop(adap->hwadapnr);
	return ret;

}

int sunxi_i2c_write(struct i2c_adapter *adap, uint8_t chip,
				uint32_t addr, int alen, uint8_t *buffer, int len)
{
	int ret;

	ret = twi_start(adap->hwadapnr);
	if (ret)
		goto i2c_write_err_occur;

	ret = twi_send_slave_addr(adap->hwadapnr, chip, I2C_WRITE);
	if (ret)
		goto i2c_write_err_occur;

	ret = twi_send_addr(adap->hwadapnr, addr, alen);
	if (ret)
		goto i2c_write_err_occur;

	ret = twi_send_data(adap->hwadapnr, buffer, len);
	if (ret)
		goto i2c_write_err_occur;

i2c_write_err_occur:
	twi_stop(adap->hwadapnr);
	return ret;

}

int sunxi_i2c_probe(struct i2c_adapter *adap, uint8_t chip)
{
	int ret;
	uint8_t buf = 0;

	ret = sunxi_i2c_read(adap, chip, 0x03, 1, &buf, 1);
	if(!ret) {
		printf("probe %x: %x\n", chip, buf);
	}
	return 0;
}

static uint sunxi_i2c_setspeed(struct i2c_adapter *adap, uint speed)
{
	i2c_set_clock(adap->hwadapnr, speed);
	adap->speed	= speed;
	return 0;
}

static struct sunxi_twi_reg *sunxi_get_r_twi_base(struct i2c_adapter *adap)
{
	return (struct sunxi_twi_reg *)(SUNXI_R_TWI_BASE);
}

#define PL_BASE			(0x07022000)
void sunxi_r_i2c_bus_setting(void)
{
	int reg_value = 0;
	volatile unsigned int reg_val;

	reg_val = readl(SUNXI_RTWI0_BRG_REG);
	/* R_TWI reset deassert*/
	reg_val |= 1 << SUNXI_RTWI0_RST_BIT;
	/* R_TWI gating open*/
	reg_val |= 1 << SUNXI_RTWI0_GATING_BIT;
	writel(reg_val, SUNXI_RTWI0_BRG_REG);

	/*R_GPIO: PL0,PL1 cfg 3*/
	writel(readl(PL_BASE)& ~0xff,PL_BASE);
	writel(readl(PL_BASE)|0x33,PL_BASE);
	/*PL0,PL1 pull up 1*/
	writel(readl(PL_BASE+0x1C)& ~0xf,PL_BASE+0x1C);
	writel(readl(PL_BASE+0x1C)|0x5,PL_BASE+0x1C);
	/*PL0,PL1 drv 2*/
	writel(readl(PL_BASE+0x14)& ~0xf,PL_BASE+0x14);
	writel(readl(PL_BASE+0x14)|0x0,PL_BASE+0x14);

	/*deassert twi reset*/
	reg_value = readl(SUNXI_RTWI0_BRG_REG);
	reg_value &= ~(0x01 << SUNXI_RTWI0_RST_BIT);
	writel(reg_value,SUNXI_RTWI0_BRG_REG);
	reg_value = readl(SUNXI_RTWI0_BRG_REG);
	reg_value |= 0x01 << SUNXI_RTWI0_RST_BIT;
	writel(reg_value,SUNXI_RTWI0_BRG_REG);
	__usdelay(10);

	/*open twi gating*/
	reg_value = readl(SUNXI_RTWI0_BRG_REG);
	reg_value |= 0x01 << SUNXI_RTWI0_GATING_BIT;
	writel(reg_value,SUNXI_RTWI0_BRG_REG);
	__usdelay(10);
}

void sunxi_r_i2c_init(struct i2c_adapter *adap, int speed, int slaveaddr)
{
	
	if (sunxi_i2c[adap->hwadapnr] != NULL) {
		I2C_DBG("[I2C-WRN]:i2c%d has been initialized\n", adap->hwadapnr);
		goto OUT;
	}

	sunxi_i2c[adap->hwadapnr] = sunxi_get_r_twi_base(adap);

	sunxi_r_i2c_bus_setting();
	sunxi_i2c_setspeed(adap, speed);

OUT:
	I2C_DBG("i2c%d info:%x(slaveaddr),%d(speed)\n", adap->hwadapnr,
	       adap->slaveaddr, adap->speed);
	       
}
/*
U_BOOT_I2C_ADAP_COMPLETE(sunxi_r_i2c0, sunxi_r_i2c_init, sunxi_i2c_probe,
			 sunxi_i2c_read, sunxi_i2c_write,
			 sunxi_i2c_setspeed,
			 CONFIG_SYS_I2C_SPEED,
			 CONFIG_SYS_I2C_SLAVE,
			 4)
*/
