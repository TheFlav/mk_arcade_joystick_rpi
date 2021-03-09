/*
 *  Arcade Joystick Driver for RaspberryPi
 *
 *  Copyright (c) 2014 Matthieu Proucelle
 *
 *  Based on the gamecon driver by Vojtech Pavlik, and Markus Hiienkari
 */


/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <linux/i2c.h>

#include <linux/ioport.h>
#include <asm/io.h>

#include <linux/jiffies.h>


MODULE_AUTHOR("Matthieu Proucelle (edited for Freeplaytech by Ed Mandy)");
MODULE_DESCRIPTION("Freeplay GPIO Arcade Joystick Driver");
MODULE_LICENSE("GPL");


// MK
#define MK_MAX_DEVICES  2
#define MK_MAX_BUTTONS  21 //13
static const char *mk_names[] = {NULL, "GPIO Controller 1", "GPIO Controller 2", "MCP23017 Controller", "GPIO Controller 1" , "GPIO Controller 1", "GPIO Controller 2"};

enum mk_type {
    MK_NONE = 0,
    MK_ARCADE_GPIO,
    MK_ARCADE_GPIO_BPLUS,
    MK_ARCADE_GPIO_TFT,
    MK_ARCADE_GPIO_CUSTOM,
    MK_ARCADE_GPIO_CUSTOM2,
    MK_MAX
};

struct mk_pad {
    struct input_dev *dev;
    enum mk_type type;
    char phys[32];
    int hotkey_mode;
    int gpio_maps[MK_MAX_BUTTONS];
};

struct mk {
    struct mk_pad pads[MK_MAX_DEVICES];
    struct timer_list timer;
    int used;
    struct mutex mutex;
    int total_pads;
};

static struct mk *mk_base;

struct mk_subdev {unsigned int idx;};

#define MK_REFRESH_TIME    HZ/100

struct mk_nin_gpio {
    unsigned pad_id;
    unsigned cmd_setinputs;
    unsigned cmd_setoutputs;
    unsigned valid_bits;
    unsigned request;
    unsigned request_len;
    unsigned response_len;
    unsigned response_bufsize;
};

struct mk_config {
    int args[MK_MAX_DEVICES];
    unsigned int nargs;
};

static struct mk_config mk_cfg __initdata;
module_param_array_named(map, mk_cfg.args, int, &(mk_cfg.nargs), 0);
MODULE_PARM_DESC(map, "Enable or disable GPIO, TFT and Custom Arcade Joystick");


// GPIO
#ifdef RPI2
#define PERI_BASE  0x3F000000
#elif defined(RPI4)
#define PERI_BASE  0xFE000000
#else
#define PERI_BASE  0x20000000
#endif

#define GPIO_BASE  (PERI_BASE + 0x200000) /* GPIO controller */

#define INP_GPIO(g)  *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3)) //set GPIO as input
#define OUT_GPIO(g)  *(gpio+((g)/10)) |=  (1<<(((g)%10)*3)) //set GPIO as output
#define GPIO_READ(g)  ((g < 32) ? (*(gpio + 13) &= (1<<(g))) : (*(gpio + 14) &= (1<<(g-32)))) //work >= 32

#define GET_GPIO(g)  (*(gpio.addr + BCM2835_GPLEV0/4)&(1<<g)) // 0 if LOW, (1<<g) if HIGH

#define SET_GPIO_ALT(g,a)  *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3)) //set GPIO alt function

#define GPIO_SET(g)  ((g < 32) ? (*(gpio + 7) = (1<<(g))) : (*(gpio + 8) = (1<<(g-32)))) //work >= 32
#define GPIO_CLR(g)  ((g < 32) ? (*(gpio + 10) = (1<<(g))) : (*(gpio + 11) = (1<<(g-32)))) //work >= 32

struct gpio_config {
    int mk_arcade_gpio_maps_custom[MK_MAX_BUTTONS];
    unsigned int nargs;
};

static struct gpio_config gpio_cfg __initdata; // for player 1
module_param_array_named(gpio, gpio_cfg.mk_arcade_gpio_maps_custom, int, &(gpio_cfg.nargs), 0);
MODULE_PARM_DESC(gpio, "Numbers of custom GPIO for Arcade Joystick 1");

static struct gpio_config gpio_cfg2 __initdata; // for player 2
module_param_array_named(gpio2, gpio_cfg2.mk_arcade_gpio_maps_custom, int, &(gpio_cfg2.nargs), 0);
MODULE_PARM_DESC(gpio2, "Numbers of custom GPIO for Arcade Joystick 2");

static volatile unsigned *gpio;
unsigned char data[MK_MAX_BUTTONS];     //so we always keep the state of data

// Map of the gpios :                     up, down, left, right, start, select, a,  b,  tr, y,  x,  tl, hk, l2, r2, c,  z
static const int mk_arcade_gpio_maps[] = {4,  17,    27,  22,    10,    9,      25, 24, 23, 18, 15, 14, 2 , -1, -1, -1, -1};
// 2nd joystick on the b+ GPIOS                 up, down, left, right, start, select, a,  b,  tr, y,  x,  tl, hk, l2, r2, c,  z
static const int mk_arcade_gpio_maps_bplus[] = {11, 5,    6,    13,    19,    26,     21, 20, 16, 12, 7,  8,  3,  -1, -1, -1, -1};

// Map joystick on the b+ GPIOS with TFT      up, down, left, right, start, select, a,  b,  tr, y,  x,  tl, hk, l2, r2, c,  z
static const int mk_arcade_gpio_maps_tft[] = {21, 13,    26,    19,    5,    6,     22, 4, 20, 17, 27,  16, 12, -1, -1, -1, -1};
static const short mk_arcade_gpio_btn[] = {BTN_START, BTN_SELECT, BTN_A, BTN_B, BTN_TR, BTN_Y, BTN_X, BTN_TL, BTN_MODE /*this one can be special*/, BTN_TL2, BTN_TR2, BTN_C, BTN_Z, BTN_TOP, BTN_TOP2, BTN_BASE, BTN_BASE2};


// Hotkey
struct hkmode_config {
    int mode[1];   //HOTKEY_MODE_*
    unsigned int nargs;
};
static struct hkmode_config hkmode_cfg __initdata;
module_param_array_named(hkmode, hkmode_cfg.mode, int, &(hkmode_cfg.nargs), 0);
MODULE_PARM_DESC(hkmode, "Hotkey Button Mode: 1=NORMAL, 2=TOGGLE");
unsigned char hk_state_prev = 0xFF;
unsigned char hk_pre_mode = 0;
int hotkey_combo_btn = -1;
#define HOTKEY_MODE_UNDEFINED   0
#define HOTKEY_MODE_NORMAL      1
#define HOTKEY_MODE_TOGGLE      2


// I2C Bus
struct i2cbus_config {
    int busnum[1];   //HOTKEY_MODE_*
    unsigned int nargs;
};

static struct i2cbus_config i2cbus_cfg __initdata;
module_param_array_named(i2cbus, i2cbus_cfg.busnum, int, &(i2cbus_cfg.nargs), 0);
MODULE_PARM_DESC(i2cbus, "I2C Bus Number /dev/i2c-# (typically 0 or 1)");
struct i2c_adapter* i2c_dev = NULL;


// I2C
struct analog_config {
    int address[1];   //i2c address of the adc
    unsigned int nargs;
};

static struct analog_config analog_x1_cfg __initdata;
module_param_array_named(x1addr, analog_x1_cfg.address, int, &(analog_x1_cfg.nargs), 0);
MODULE_PARM_DESC(x1addr, "I2C address of X1 ADC MCP3021A chip");
struct i2c_client* i2c_client_x1 = NULL;
bool x1_enable = false; //nns: x1 enabled?

static struct analog_config analog_y1_cfg __initdata;
module_param_array_named(y1addr, analog_y1_cfg.address, int, &(analog_y1_cfg.nargs), 0);
MODULE_PARM_DESC(y1addr, "I2C address of Y1 ADC MCP3021A chip");
struct i2c_client* i2c_client_y1 = NULL;
bool y1_enable = false; //nns: y1 enabled?

static struct analog_config analog_x2_cfg __initdata;
module_param_array_named(x2addr, analog_x2_cfg.address, int, &(analog_x2_cfg.nargs), 0);
MODULE_PARM_DESC(x2addr, "I2C address of X2 ADC MCP3021A chip");
struct i2c_client* i2c_client_x2 = NULL;
bool x2_enable = false; //nns: x2 enabled?

static struct analog_config analog_y2_cfg __initdata;
module_param_array_named(y2addr, analog_y2_cfg.address, int, &(analog_y2_cfg.nargs), 0);
MODULE_PARM_DESC(y2addr, "I2C address of Y2 ADC MCP3021A chip");
struct i2c_client* i2c_client_y2 = NULL;
bool y2_enable = false; //nns: y2 enabled?


// I2C ADS1015
struct ads1015_config { //nns: add ads1015 support
    int address[1];   //i2c address of the adc
    unsigned int nargs;
};

static struct ads1015_config ads1015_cfg __initdata;
module_param_array_named(ads1015addr, ads1015_cfg.address, int, &(ads1015_cfg.nargs), 0);
MODULE_PARM_DESC(ads1015addr, "I2C address of ADC ADS1015 chip");
bool ads1015_enable = false; //nns: ads1015 enabled?
int ads1015_lookup[] = {-1,-1,-1,-1}; //ads1015 ain x1,y1,x2,y2 lookup table
uint16_t ads1015_ain[] = {0x4000,0x5000,0x6000,0x7000}; //ain0,ain1,ain2,ain3 value for bitwise operation


// Analog Auto Center
struct auto_center_config {
    int auto_center[1];
    unsigned int nargs;
};

static struct auto_center_config auto_center_cfg __initdata;
module_param_array_named(auto_center_analog, auto_center_cfg.auto_center, int, &(auto_center_cfg.nargs), 0);
MODULE_PARM_DESC(auto_center_analog, "Use auto centering for analog sticks (1=yes, 0=no)");
bool auto_center = false; //nns: analog center offcenter
int16_t x1_offset = 2048; //nns: use for analog center offcenter
int16_t y1_offset = 2048; //nns: use for analog center offcenter
int16_t x2_offset = 2048; //nns: use for analog center offcenter
int16_t y2_offset = 2048; //nns: use for analog center offcenter


// Analog Direction
struct analog_direction_config { //nns: add analog direction
    int dir[1];
    unsigned int nargs;
};

static struct analog_direction_config analog_x1_direction_cfg __initdata;
module_param_array_named(x1dir, analog_x1_direction_cfg.dir, int, &(analog_x1_direction_cfg.nargs), 0);
MODULE_PARM_DESC(x1dir, "X1 analog direction");
bool x1_reverse = false; //nns: x1 reverse direction

static struct analog_direction_config analog_y1_direction_cfg __initdata;
module_param_array_named(y1dir, analog_y1_direction_cfg.dir, int, &(analog_y1_direction_cfg.nargs), 0);
MODULE_PARM_DESC(y1dir, "Y1 analog direction");
bool y1_reverse = false; //nns: y1 reverse direction

static struct analog_direction_config analog_x2_direction_cfg __initdata;
module_param_array_named(x2dir, analog_x2_direction_cfg.dir, int, &(analog_x2_direction_cfg.nargs), 0);
MODULE_PARM_DESC(x2dir, "X2 analog direction");
bool x2_reverse = false; //nns: x2 reverse direction

static struct analog_direction_config analog_y2_direction_cfg __initdata;
module_param_array_named(y2dir, analog_y2_direction_cfg.dir, int, &(analog_y2_direction_cfg.nargs), 0);
MODULE_PARM_DESC(y2dir, "Y2 analog direction");
bool y2_reverse = false; //nns: y2 reverse direction


// GPIO based Force Feedback
struct ff_config { //nns: gpio force feedback support
    int pins[2];   //gpio pin to use
    unsigned int nargs;
};

static struct ff_config ff_cfg __initdata;
module_param_array_named(ff, ff_cfg.pins, int, &(ff_cfg.nargs), 0);
MODULE_PARM_DESC(ff, "Force feedback parameters (gpio pin for strong rumble, gpio pin for weak rumble), negative value for reverse logic");
bool ff_enable=false; //gpio force feedback enable
int ff_gpio_strong_pin=-1; //rumble strong pin
int ff_gpio_weak_pin=-1; //rumble weak pin
bool ff_effect_strong_reverse=false; //rumble strong reverse logic
bool ff_effect_weak_reverse=false; //rumble weak reverse logic
bool ff_effect_strong_running=false; //rumble strong running
bool ff_effect_weak_running=false; //rumble weak running


// GPIO based Force Feedback Direction
struct ffdir_config { //nns: gpio force feedback direction support
    int pins[2];   //gpio pin to use
    unsigned int nargs;
};

static struct ffdir_config ffdir_cfg __initdata;
module_param_array_named(ffdir, ffdir_cfg.pins, int, &(ffdir_cfg.nargs), 0);
MODULE_PARM_DESC(ffdir, "Force feedback direction parameters (gpio pin for rumble direction), negative value for reverse logic");
bool ff_dir_enable=false; //force feedback direction enable
int ff_gpio_dir_pin=-1; //rumble direction pin
bool ff_gpio_dir_reverse=false; //rumble direction reverse logic
unsigned int ff_effect_dir=0; //rumble direction


// I2C PCA9633 based PWM Force Feedback
struct ffpwm_config { //nns: pwm force feedback support
    int params[3];   //gpio pin to use
    unsigned int nargs;
};

static struct ffpwm_config ffpwm_cfg __initdata;
module_param_array_named(ffpwm, ffpwm_cfg.params, int, &(ffpwm_cfg.nargs), 0);
MODULE_PARM_DESC(ffpwm, "Force feedback PWM parameters, require PCA9633 (PCA9633 adress, output for strong rumble, output for weak rumble), negative value for reverse logic");
bool ff_pwm_enable=false; //pwm force feedback enable
struct i2c_client* pca9633_client=NULL; //PCA9633
unsigned int pca9633_ledout; //PCA9633 initial LEDOUT register
unsigned int pca9633_ledout_backup; //PCA9633 initial LEDOUT register backup
int ff_strong_pwm=-1; //rumble strong PCA9633 pwm output
int ff_weak_pwm=-1; //rumble weak PCA9633 pwm output
bool ff_strong_pwm_reverse=false; //rumble strong pwm reverse logic
bool ff_weak_pwm_reverse=false; //rumble weak pwm reverse logic
bool ff_strong_pwm_sent=true; //rumble strong pwm already sent via i2c
bool ff_weak_pwm_sent=true; //rumble weak pwm already sent via i2c
int ff_strong_pwm_value=0; //rumble strong PCA9633 pwm value
int ff_weak_pwm_value=0; //rumble weak PCA9633 pwm value



// Debug
struct debug_config { //nns: add debug
    int debug[1];
    unsigned int nargs;
};

static struct debug_config debug_config_cfg __initdata;
module_param_array_named(debug, debug_config_cfg.debug, int, &(debug_config_cfg.nargs), 0);
MODULE_PARM_DESC(debug, "Debug level, 0:disable, 1:event, 2:loop");
unsigned int debug_mode=0; //debug level, 0:disable, 1:event, 2:loop
unsigned int benchmark_maxloop=200; //loop count before report, may be removed in the future
unsigned int benchmark_loop=0; //loop count, may be removed in the future
unsigned int benchmark_time=0; //duration, may be removed in the future
unsigned int benchmark_tmp=0; //tmp, may be removed in the future
unsigned long benchmark_time_start=0; //loop start time, may be removed in the future
unsigned long jiffies_last=0; //backup to check jiffies rollover, may be removed in the future



// Analog axis parameters
struct analog_abs_params_config { //add analog parameters
    int abs_params[4];
    unsigned int nargs;
};

static struct analog_abs_params_config analog_x1_abs_params_cfg __initdata;

module_param_array_named(x1params, analog_x1_abs_params_cfg.abs_params, int, &(analog_x1_abs_params_cfg.nargs), 0);
MODULE_PARM_DESC(x1params, "X1 ADC absolute parameters (min,max,fuzz,flat)");
#define ABS_PARAMS_DEFAULT_X_MIN 374
#define ABS_PARAMS_DEFAULT_X_MAX 3418
#define ABS_PARAMS_DEFAULT_X_FUZZ 16
#define ABS_PARAMS_DEFAULT_X_FLAT 384
uint16_t x1_max = 0;
uint16_t x1_min = 0xFFFF;

static struct analog_abs_params_config analog_y1_abs_params_cfg __initdata;
module_param_array_named(y1params, analog_y1_abs_params_cfg.abs_params, int, &(analog_y1_abs_params_cfg.nargs), 0);
MODULE_PARM_DESC(y1params, "Y1 ADC absolute parameters (min,max,fuzz,flat)");
#define ABS_PARAMS_DEFAULT_Y_MIN 517
#define ABS_PARAMS_DEFAULT_Y_MAX 3378
#define ABS_PARAMS_DEFAULT_Y_FUZZ 16
#define ABS_PARAMS_DEFAULT_Y_FLAT 384
uint16_t y1_max = 0;
uint16_t y1_min = 0xFFFF;

static struct analog_abs_params_config analog_x2_abs_params_cfg __initdata;
module_param_array_named(x2params, analog_x2_abs_params_cfg.abs_params, int, &(analog_x2_abs_params_cfg.nargs), 0);
MODULE_PARM_DESC(x2params, "X2 ADC absolute parameters (min,max,fuzz,flat)");
uint16_t x2_max = 0;
uint16_t x2_min = 0xFFFF;

static struct analog_abs_params_config analog_y2_abs_params_cfg __initdata;
module_param_array_named(y2params, analog_y2_abs_params_cfg.abs_params, int, &(analog_y2_abs_params_cfg.nargs), 0);
MODULE_PARM_DESC(y2params, "Y2 ADC absolute parameters (min,max,fuzz,flat)");
uint16_t y2_max = 0;
uint16_t y2_min = 0xFFFF;

struct analog_abs_params_struct {int min, max, fuzz, flat;} x1_analog_abs_params, x2_analog_abs_params, y1_analog_abs_params, y2_analog_abs_params;



struct delayed_work mk_delayed_work;
struct mk *g_mk = NULL;

static struct i2c_board_info __initdata board_info[] = {{I2C_BOARD_INFO("MCP3021X1", 0x48),}};

struct i2c_client *i2c_new_MCP3021(struct i2c_adapter *adapter, u16 address){
    struct i2c_board_info info = {I2C_BOARD_INFO("MCP3021", address),};
    return i2c_new_device(adapter, &info);
}

struct i2c_client *i2c_new_ADS1015(struct i2c_adapter *adapter, u16 address){ //nns: add ADS1015 support
    struct i2c_board_info info = {I2C_BOARD_INFO("ADS1015", address),};
    return i2c_new_device(adapter, &info);
}

struct i2c_client *i2c_new_PCA9633(struct i2c_adapter *adapter, u16 address){ //nns: add PCA9633 support
    struct i2c_board_info info = {I2C_BOARD_INFO("PCA9633", address),};
    return i2c_new_device(adapter, &info);
}



static int16_t ADC_OffsetCenter(uint16_t adc_resolution,uint16_t adc_value,uint16_t adc_min,uint16_t adc_max,int16_t adc_offset){
    int16_t adc_center; int16_t range; int32_t ratio; int16_t corrected_value; //used variables
    adc_center=adc_resolution/2; //center value, 2048 for 12bits
    if(adc_value<(adc_center+adc_offset)){ //value under center offset
        range=(adc_center+adc_offset)-adc_min;
        if(range!=0){ //to avoid divide by 0
            ratio=10000*adc_center/range; //float workaround
            corrected_value=(adc_value-adc_min)*ratio/10000;
        }else{corrected_value=adc_value;} //range=0, setting problems?
    }else{ //value over center offset
        range=adc_max-(adc_center+adc_offset);
        if(range!=0){ //to avoid divide by 0
            ratio=10000*adc_center/range; //float workaround
            corrected_value=adc_center+(adc_value-(adc_center+adc_offset))*ratio/10000;
        }else{corrected_value=adc_value;} //range=0, setting problems?
    }
    
    if(corrected_value<1){corrected_value=1;}else if(corrected_value>4094){corrected_value=4094;} //constrain computed value to 12bits value + fix for Reicast overflow
    return corrected_value;
}


static int16_t ADC_Deadzone(uint16_t adc_value,uint16_t min,uint16_t max,uint16_t flat){
    int16_t adc_center; //used variables
    adc_center=(max+min)/2; //center value, 2048 for 12bits
    if(adc_value>adc_center-flat&&adc_value<adc_center+flat){adc_value=adc_center;} //apply flat value to adc value
    return adc_value;
}


static int16_t ADS1015_read(const struct i2c_client *client,uint16_t axis){ //based on https://github.com/torvalds/linux/blob/master/drivers/hwmon/ads1015.c
    int16_t value=0; //used variables
    int16_t ain=ads1015_lookup[axis]; //get ain id
    if(ain<0||ain>3){return -1;} //fail: ain oob, return -1
    i2c_smbus_write_word_swapped(client,0x01,0x83E3|ads1015_ain[ain]); //default config but +-4.096v FSR and 3300SPS
    udelay(450); //wait 450us for conversion, 390us sould be enough in worst case but +60us add a extra security
    value=i2c_smbus_read_word_swapped(client,0); //read value
    if(value>=0){ //success
        value=value>>4; //shift bits to get 12bits value
        if(value<=0xFFF){ //valid 12bit range
            return value; //return adc result
        }else{return -1;} //invalid range
    }else{return value;} //fail: return i2c error
}

/* GPIO UTILS */
#if defined(RPI4)

#define PULL_UNSET  -1
#define PULL_NONE    0
#define PULL_DOWN    1
#define PULL_UP      2

/* 2711 has a different mechanism for pin pull-up/down/enable  */
#define GPPUPPDN0    57        /* Pin pull-up/down for pins 15:0  */
#define GPPUPPDN1    58        /* Pin pull-up/down for pins 31:16 */
#define GPPUPPDN2    59        /* Pin pull-up/down for pins 47:32 */
#define GPPUPPDN3    60        /* Pin pull-up/down for pins 57:48 */




static int bcm2711_set_pull(unsigned int gpioPin, int type)
{

    int pullreg = GPPUPPDN0 + (gpioPin>>4);
    int pullshift = (gpioPin & 0xf) << 1;
    unsigned int pullbits;
    unsigned int pull;
    
    //printk("bcm2711_set_pull gpioPin=%d type=%d\n", gpioPin, type);

    switch (type)
    {
    case PULL_NONE:
        pull = 0;
        break;
    case PULL_UP:
        pull = 1;
        break;
    case PULL_DOWN:
        pull = 2;
        break;
    default:
        return 1; /* An illegal value */
    }

    pullbits = *(gpio + pullreg);
    pullbits &= ~(3 << pullshift);
    pullbits |= (pull << pullshift);
    *(gpio + pullreg) = pullbits;

    //printk("bcm2711_set_pull set 0x%08X to *(0x%08X + 0x%08X)\n", pullbits, gpio, pullreg);

    
    return 0;
}

static void setGpioPullUps(uint32_t pullUps, uint32_t pullUpsHigh)
{
    printk("setGpioPullUps RPi 4\n");

    int gpio;


  for(gpio=0; gpio < 32; gpio++)
  {
    if(pullUps & (1 << gpio))
      bcm2711_set_pull(gpio, PULL_UP);
      
    if(pullUpsHigh & (1 << gpio))
      bcm2711_set_pull(gpio + 32, PULL_UP);
  }

}
#else
static void setGpioPullUps(uint32_t pullUps, uint32_t pullUpsHigh){
    printk("setGpioPullUps RPi 3 and below\n");
    
    *(gpio + 37) = 0x02;
    udelay(10);
    *(gpio + 38) = pullUps;
    *(gpio + 39) = pullUpsHigh;
    udelay(10);
    *(gpio + 37) = 0x00;
    *(gpio + 38) = 0x00;
    *(gpio + 39) = 0x00;
}
#endif

static void setGpioAsInput(int gpioNum){INP_GPIO(gpioNum);} //gpio: set as input
static void setGpioAsOuput(int gpioNum){OUT_GPIO(gpioNum);} //gpio: set as output
static void GpioOuputSet(int gpioNum){GPIO_SET(gpioNum);} //gpio: set output high
static void GpioOuputClr(int gpioNum){GPIO_CLR(gpioNum);} //gpio: set output low
    
    

static int getPullUpMask(int gpioMap[], uint32_t *maskLow, uint32_t *maskHigh){
    int i;
    *maskLow = 0x0000000;
    *maskHigh = 0x0000000;
    
    for(i=0; i<MK_MAX_BUTTONS;i++){
        if(gpioMap[i] > -1){ //to avoid unused pins
            if(gpioMap[i] < 32){
                *maskLow |= 1<<gpioMap[i];
            }else if(gpioMap[i] < 64){
                *maskHigh |= 1<<(gpioMap[i] - 32);
            }
        }
    }
    
    //printk("mask low=0x%08X high=0x%08X\n", *maskLow, *maskHigh);
    return *maskLow;
}


static void mk_gpio_read_packet(struct mk_pad * pad, unsigned char *data){
    int i;
    
    for(i = 0; i < MK_MAX_BUTTONS; i++){
        if(pad->gpio_maps[i] != -1){    // to avoid unused buttons
            if((i==12) && (pad->hotkey_mode == HOTKEY_MODE_TOGGLE)){  //the hotkey
                //we use the hotkey as a toggle (press to toggle data[i])
                unsigned char hk_state;
                int read;
                if(pad->gpio_maps[i] < 0){
                    read = GPIO_READ(pad->gpio_maps[i] * -1); //invert this signal
                    if(read == 0){
                        hk_state = 0;
                    }else{
                        hk_state = 1; //pressed
                    }
                }else{
                    read = GPIO_READ(pad->gpio_maps[i]);
                    if(read == 0){
                        hk_state = 1; //pressed
                    }else{
                        hk_state = 0;
                    }
                }
                
                if(hk_state != hk_state_prev){ //the hotkey changed
                    hk_state_prev = hk_state;
                    
                    //if it changed and it's now a 1, we enter pre-hotkey mode
                    if(hk_state){
                        if(hk_pre_mode){
                            //the PWR btn itself is the hotkey
                            data[12] = 1;   //turn on the hotkey
                            hotkey_combo_btn = i;
                        }else{
                            hk_pre_mode = 1;
                            hotkey_combo_btn = -1;
                        }
                    }else if(hotkey_combo_btn == i){  //the hotkey was just released, and the PWR btn itself is the hotkey
                        data[12] = 0;   //turn off the hotkey
                        hk_pre_mode = 0;
                        hotkey_combo_btn = -1;
                    }
                }
            }else{
                //all other (non-hotkey) buttons just report their state to data[i]
                //except when we are in hk_state
                unsigned char prev_data = data[i];
                if(pad->gpio_maps[i] < 0){ //invert this signal
                    int read = GPIO_READ(pad->gpio_maps[i] * -1);
                    if(read == 0){
                        data[i] = 0;
                    }else{data[i] = 1;}
                }else{
                    int read = GPIO_READ(pad->gpio_maps[i]);
                    if(read == 0){
                        data[i] = 1;
                    }else{data[i] = 0;}
                }
                
                if(prev_data != data[i]){ //the state of this button changed
                    if(hk_pre_mode){
                        if(data[i]){ //the button was just pressed
                            data[12] = 1;   //turn on the hotkey
                            hotkey_combo_btn = i;
                        }else if(i == hotkey_combo_btn){   //the button was just released
                            data[12] = 0;   //turn off the hotkey
                            hk_pre_mode = 0;
                            hotkey_combo_btn = -1;
                        }
                    }
                }
            }
        }else{data[i] = 0;}
    }
}


static void mk_input_report(struct mk_pad * pad, unsigned char * data){
    if(debug_mode>1){benchmark_time_start=(unsigned long)jiffies;} //benchmark, may be removed in the future
    
    struct input_dev * dev = pad->dev;
    int j; //gpio maps loop
    int16_t adc_val = 2048; //security if something goes wrong
    
    if(x1_enable){input_report_abs(dev, ABS_HAT0X, !data[2]-!data[3]); //if using analog, DPAD is ABS_HAT0X
    }else{input_report_abs(dev, ABS_X, !data[2]-!data[3]);} //DPAD is ABS_X
    
    if(y1_enable){input_report_abs(dev, ABS_HAT0Y, !data[0]-!data[1]); //if using analog, DPAD is ABS_HAT0Y
    }else{input_report_abs(dev, ABS_Y, !data[0]-!data[1]);} //DPAD is ABS_Y
    
    if(x1_enable){ //if using analog for x1
        if(ads1015_enable){adc_val = ADS1015_read(i2c_client_x1,0); //ads1015
        }else{adc_val = i2c_smbus_read_word_swapped(i2c_client_x1,0);} //mcp3021
        if(adc_val>=0){
            if(x1_reverse){adc_val = abs(4096-adc_val);} //nns: reverse 12bits value
            if(adc_val < x1_min){x1_min = adc_val;} //update x1 analog min value
            if(adc_val > x1_max){x1_max = adc_val;} //update x1 analog max value
            adc_val = ADC_OffsetCenter(4096,adc_val,x1_analog_abs_params.min,x1_analog_abs_params.max,x1_offset); //re-center adc value
            adc_val = ADC_Deadzone(adc_val,0x000,0xFFF,x1_analog_abs_params.flat); //apply flat value to adc value
            input_report_abs(dev, ABS_X, adc_val);
        }else if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : failed to read analog X1, returned %i\n",adc_val);} //nns: debug
    }
    
    if(y1_enable){ //if using analog for y1
        if(ads1015_enable){adc_val = ADS1015_read(i2c_client_x1,1); //ads1015
        }else{adc_val = i2c_smbus_read_word_swapped(i2c_client_y1,0);} //mcp3021
        if(adc_val>=0){
            if(y1_reverse){adc_val = abs(4096-adc_val);} //nns: reverse 12bits value
            if(adc_val < y1_min){y1_min = adc_val;} //update y1 analog min value
            if(adc_val > y1_max){y1_max = adc_val;} //update y1 analog max value
            adc_val = ADC_OffsetCenter(4096,adc_val,y1_analog_abs_params.min,y1_analog_abs_params.max,y1_offset); //re-center adc value
            adc_val = ADC_Deadzone(adc_val,0x000,0xFFF,y1_analog_abs_params.flat); //apply flat value to adc value
            input_report_abs(dev, ABS_Y, adc_val);
        }else if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : failed to read analog Y1, returned %i\n",adc_val);} //nns: debug
    }
    
    if(x2_enable){ //if using analog for x2
        if(ads1015_enable){adc_val = ADS1015_read(i2c_client_x1,2); //ads1015
        }else{adc_val = i2c_smbus_read_word_swapped(i2c_client_x2,0);} //mcp3021
        if(adc_val>=0){
            if(x2_reverse){adc_val = abs(4096-adc_val);} //nns: reverse 12bits value
            if(adc_val < x2_min){x2_min = adc_val;} //update x2 analog min value
            if(adc_val > x2_max){x2_max = adc_val;} //update x2 analog max value
            adc_val = ADC_OffsetCenter(4096,adc_val,x2_analog_abs_params.min,x2_analog_abs_params.max,x2_offset); //re-center adc value
            adc_val = ADC_Deadzone(adc_val,0x000,0xFFF,x2_analog_abs_params.flat); //apply flat value to adc value
            input_report_abs(dev, ABS_RX, adc_val);
        }else if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : failed to read analog X2, returned %i\n",adc_val);} //nns: debug
    }
    
    if(y2_enable){ //if using analog for x2
        if(ads1015_enable){adc_val = ADS1015_read(i2c_client_x1,3); //ads1015
        }else{adc_val = i2c_smbus_read_word_swapped(i2c_client_y2,0);} //mcp3021
        if(adc_val>=0){
            if(y2_reverse){adc_val = abs(4096-adc_val);} //nns: reverse 12bits value
            if(adc_val < y2_min){y2_min = adc_val;} //update y2 analog min value
            if(adc_val > y2_max){y2_max = adc_val;} //update y2 analog max value
            adc_val = ADC_OffsetCenter(4096,adc_val,y2_analog_abs_params.min,y2_analog_abs_params.max,y2_offset); //re-center adc value
            adc_val = ADC_Deadzone(adc_val,0x000,0xFFF,y2_analog_abs_params.flat); //apply flat value to adc value
            input_report_abs(dev, ABS_RY, adc_val);
        }else if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : failed to read analog Y2, returned %i\n",adc_val);} //nns: debug
    }
    
    for (j = 4; j < MK_MAX_BUTTONS; j++){
        if(pad->gpio_maps[j] != -1){input_report_key(dev, mk_arcade_gpio_btn[j - 4], data[j]);}
    }
    
    input_sync(dev);
    
    //PWM force feedback, need to be here because i2c_smbus_write_byte_data mess with schedule_delayed_work
    if(!ff_strong_pwm_sent){ //pwm i2c not already sent
        if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : Feedback effect : PWM Strong : %d\n",ff_strong_pwm_value);}
        i2c_smbus_write_byte_data(pca9633_client,(uint8_t)(ff_strong_pwm+2),(uint8_t)(ff_strong_pwm_value)); //send pwm to i2c
        ff_strong_pwm_sent=true; //reset
    }
    
    if(!ff_weak_pwm_sent){ //pwm i2c not already sent
        if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : Feedback effect : PWM Weak : %d\n",ff_weak_pwm_value);}
        i2c_smbus_write_byte_data(pca9633_client,(uint8_t)(ff_weak_pwm+2),(uint8_t)(ff_weak_pwm_value)); //send pwm to i2c
        ff_weak_pwm_sent=true; //reset
    }
    
    if(debug_mode>1){ //may be removed in the future
        if(jiffies_last<(unsigned long)jiffies){ //no jiffies rollover
            benchmark_loop++;
            benchmark_tmp=benchmark_time_start-(unsigned long)jiffies; //current loop duration
            if(benchmark_tmp>benchmark_time){benchmark_time=benchmark_tmp;} //backup duration
            if(benchmark_loop>benchmark_maxloop){ //report benchmark and reset
                if(benchmark_time>0){printk("mk_arcade_joystick_rpi: DEBUG : Benchmark : Longest loop over %d updates : %d jiffies (%d msec)\n",benchmark_maxloop,benchmark_time,jiffies_to_msecs(benchmark_time)); //over 0 jiffies result
                }else{printk("mk_arcade_joystick_rpi: DEBUG : Benchmark : under 1 jiffies (%d msec) over %d updates\n",jiffies_to_msecs(1),benchmark_maxloop);} //sub 1 jiffies result
                benchmark_time=0; benchmark_loop=0; //reset values
            }
        }
        jiffies_last=(unsigned long)jiffies; //backup jiffies value to check for rollover
    }
}


static void mk_process_packet(struct mk *mk){
    struct mk_pad *pad;
    int i;
    
    for(i = 0; i < mk->total_pads; i++){
        pad = &mk->pads[i];
        mk_gpio_read_packet(pad, data);     //data is now global
        mk_input_report(pad, data);
    }
}


static int mk_ff(struct input_dev *dev, void *data, struct ff_effect *effect){ //nns: handle force feedback effects
    if(effect->type!=FF_RUMBLE){
        if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : Wrong force feedback effect\n");}
        return 0;
    }else{
        if(effect->u.rumble.strong_magnitude!=0&&!ff_effect_strong_running){ //run strong
            if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : Feedback effect : Strong : start\n");}
            if(pca9633_client!=NULL&&ff_strong_pwm!=-1){ //pwm
                ff_strong_pwm_value=abs(effect->u.rumble.strong_magnitude/256);
                if(ff_strong_pwm_reverse){ff_strong_pwm_value=abs(255-ff_strong_pwm_value);} //reverse logic
                ff_strong_pwm_sent=false;
            }
            if(ff_gpio_strong_pin!=-1){ //gpio
                if(ff_effect_strong_reverse){GpioOuputClr(ff_gpio_strong_pin); //reverse logic, set low
                }else{GpioOuputSet(ff_gpio_strong_pin);} //set high
            }
            ff_effect_strong_running=true; //running
        }else{ //stop strong
            if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : Feedback effect : Strong : stop\n");}
            if(pca9633_client!=NULL&&ff_strong_pwm!=-1){ //pwm
                if(ff_strong_pwm_reverse){ff_strong_pwm_value=255;}else{ff_strong_pwm_value=0;}
                ff_strong_pwm_sent=false;
            }
            if(ff_gpio_strong_pin!=-1){GpioOuputClr(ff_gpio_strong_pin);} //set gpio output low
            ff_effect_strong_running=false; //reset
        }
        
        if(ff_gpio_weak_pin!=-1||ff_weak_pwm!=-1){
            if(effect->u.rumble.weak_magnitude!=0&&!ff_effect_weak_running){ //run weak
                if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : Feedback effect : Weak : start\n");}
                if(pca9633_client!=NULL&&ff_weak_pwm!=-1){ //pwm
                    ff_weak_pwm_value=abs(effect->u.rumble.weak_magnitude/256);
                    if(ff_weak_pwm_reverse){ff_weak_pwm_value=abs(255-ff_weak_pwm_value);} //reverse logic
                    ff_weak_pwm_sent=false;
                }
                if(ff_gpio_weak_pin!=-1){ //gpio
                    if(ff_effect_weak_reverse){GpioOuputClr(ff_gpio_weak_pin); //reverse logic, set low
                    }else{GpioOuputSet(ff_gpio_weak_pin);} //set high
                }
                ff_effect_weak_running=true; //running
            }else{ //stop weak
                if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : Feedback effect : Weak : stop\n");}
                if(pca9633_client!=NULL&&ff_weak_pwm!=-1){ //pwm
                    if(ff_strong_pwm_reverse){ff_weak_pwm_value=255;}else{ff_weak_pwm_value=0;}
                    ff_weak_pwm_sent=false;
                }
                if(ff_gpio_weak_pin!=-1){GpioOuputClr(ff_gpio_weak_pin);} //set gpio output low
                ff_effect_weak_running=false; //reset
            }
        }
        
        if(ff_dir_enable){ //direction is set for both strong and week motor at the same time
            ff_effect_dir=effect->direction; //https://elixir.bootlin.com/linux/latest/source/include/uapi/linux/input.h#L443
            if(ff_effect_dir<16384||ff_effect_dir>49152){ //assume value under left/over right as down
                if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : Feedback effect : Direction : Down (%d)\n",ff_effect_dir);}
                if(ff_gpio_dir_reverse){GpioOuputSet(ff_gpio_dir_pin); //reverse logic, set high
                }else{GpioOuputClr(ff_gpio_dir_pin);} //set low
            }else{ //assume as up
                if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : Feedback effect : Direction : Up (%d)\n",ff_effect_dir);}
                if(ff_gpio_dir_reverse){GpioOuputClr(ff_gpio_dir_pin); //reverse logic, set low
                }else{GpioOuputSet(ff_gpio_dir_pin);} //set high
            }
        }
        return 1;
    }
}


static void mk_work_handler(struct work_struct* work){
    struct mk *mk = g_mk;
    mk_process_packet(mk);
    schedule_delayed_work(&mk_delayed_work, MK_REFRESH_TIME);
}


static int mk_open(struct input_dev *dev){
    struct mk *mk = input_get_drvdata(dev);
    int err;
    
    err = mutex_lock_interruptible(&mk->mutex);
    if(err){return err;}
    if(!mk->used++){schedule_delayed_work(&mk_delayed_work, MK_REFRESH_TIME);}
    mutex_unlock(&mk->mutex);
    return 0;
}


static void mk_close(struct input_dev *dev){
    struct mk *mk = input_get_drvdata(dev);
    
    mutex_lock(&mk->mutex);
    if(!--mk->used){cancel_delayed_work_sync(&mk_delayed_work);}
    mutex_unlock(&mk->mutex);
}


static int __init mk_setup_pad(struct mk *mk, int idx, int pad_type_arg){
    struct mk_pad *pad = &mk->pads[idx];
    struct input_dev *input_dev;
    int i, pad_type;
    int err;
    pr_err("pad type requested : %d\n",pad_type_arg);
    
    pad_type = pad_type_arg;
    
    if(pad_type < 1 || pad_type >= MK_MAX){
        pr_err("Pad type %d unknown\n", pad_type);
        return -EINVAL;
    }
    
    pad->hotkey_mode = hkmode_cfg.mode[0]; //for now, the hkmode parameter is "global" to all pads
    
    if(pad_type == MK_ARCADE_GPIO_CUSTOM){ //if the device is custom, be sure to get correct pins
        if(gpio_cfg.nargs < 1){
            pr_err("Custom device needs gpio argument\n");
            return -EINVAL;
        }else if(gpio_cfg.nargs != MK_MAX_BUTTONS){
            pr_err("Invalid gpio argument\n", pad_type);
            return -EINVAL;
        }
    }
    
    if(pad_type == MK_ARCADE_GPIO_CUSTOM2){ //if the device is custom, be sure to get correct pins
        if(gpio_cfg2.nargs < 1){
            pr_err("Custom device needs gpio argument\n");
            return -EINVAL;
        }else if(gpio_cfg2.nargs != MK_MAX_BUTTONS){
            pr_err("Invalid gpio argument\n", pad_type);
            return -EINVAL;
        }
    }
    
    pr_err("pad type : %d\n",pad_type);
    pad->dev = input_dev = input_allocate_device();
    if(!input_dev){
        pr_err("Not enough memory for input device\n");
        return -ENOMEM;
    }
    
    pad->type = pad_type;
    snprintf(pad->phys, sizeof (pad->phys), "input%d", idx);
    
    if(debug_mode>0){pr_err("Running in Debug mode %d\n", debug_mode);}
    
    input_dev->name = mk_names[pad_type];
    input_dev->phys = pad->phys;
    input_dev->id.bustype = BUS_PARPORT;
    input_dev->id.vendor = 0x0001;
    input_dev->id.product = pad_type;
    input_dev->id.version = 0x0100;
    
    input_set_drvdata(input_dev, mk);
    
    input_dev->open = mk_open;
    input_dev->close = mk_close;
    
    input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
    
    mk->total_pads++;
    
    // asign gpio pins
    switch (pad_type){
        case MK_ARCADE_GPIO:
            memcpy(pad->gpio_maps, mk_arcade_gpio_maps, MK_MAX_BUTTONS *sizeof(int));
            break;
        case MK_ARCADE_GPIO_BPLUS:
            memcpy(pad->gpio_maps, mk_arcade_gpio_maps_bplus, MK_MAX_BUTTONS *sizeof(int));
            break;
        case MK_ARCADE_GPIO_TFT:
            memcpy(pad->gpio_maps, mk_arcade_gpio_maps_tft, MK_MAX_BUTTONS *sizeof(int));
            break;
        case MK_ARCADE_GPIO_CUSTOM:
            memcpy(pad->gpio_maps, gpio_cfg.mk_arcade_gpio_maps_custom, MK_MAX_BUTTONS *sizeof(int));
            break;
        case MK_ARCADE_GPIO_CUSTOM2:
            memcpy(pad->gpio_maps, gpio_cfg2.mk_arcade_gpio_maps_custom, MK_MAX_BUTTONS *sizeof(int));
            break;
    }
    
    if(x1_enable){ //if using analog, then DPAD is ABS_HAT0X
        input_set_abs_params(input_dev, ABS_HAT0X, -1, 1, 0, 0);
        input_set_abs_params(input_dev, ABS_X, 0x000, 0xFFF, x1_analog_abs_params.fuzz, x1_analog_abs_params.flat); //nns: parameters for center offcenter values
    }else{
        input_set_abs_params(input_dev, ABS_X, -1, 1, 0, 0);
    }
    
    if(y1_enable){ //if using analog, then DPAD is ABS_HAT0Y
        input_set_abs_params(input_dev, ABS_HAT0Y, -1, 1, 0, 0);
        input_set_abs_params(input_dev, ABS_Y, 0x000, 0xFFF, y1_analog_abs_params.fuzz, y1_analog_abs_params.flat); //nns: parameters for center offcenter values
    }else{
        input_set_abs_params(input_dev, ABS_Y, -1, 1, 0, 0);
    }
    
    if(x2_enable){input_set_abs_params(input_dev, ABS_RX, 0x000, 0xFFF, x2_analog_abs_params.fuzz, x2_analog_abs_params.flat);} //nns: parameters for center offcenter values
    if(y2_enable){input_set_abs_params(input_dev, ABS_RY, 0x000, 0xFFF, y2_analog_abs_params.fuzz, y2_analog_abs_params.flat);} //nns: parameters for center offcenter values
    
    for (i = 0; i < MK_MAX_BUTTONS - 4; i++){
        if(pad->gpio_maps[i+4] != -1){__set_bit(mk_arcade_gpio_btn[i], input_dev->keybit);}
    }
    
    // initialize gpio
    for (i = 0; i < MK_MAX_BUTTONS; i++){
        if(pad->gpio_maps[i] != -1){    // to avoid unused buttons
            if(pad->gpio_maps[i] < 0){
                setGpioAsInput(pad->gpio_maps[i] * -1);
            }else{
                setGpioAsInput(pad->gpio_maps[i]);
            }
        }
    }
    
    uint32_t pullUpMaskLow, pullUpMaskHigh;
    getPullUpMask(pad->gpio_maps, &pullUpMaskLow, &pullUpMaskHigh);
    
    setGpioPullUps(pullUpMaskLow, pullUpMaskHigh);
    printk("mk_arcade_joystick_rpi: GPIO configured for pad%d\n", idx);
    
    if(ff_enable||ff_pwm_enable){ //nns: force feedback support
        int ff_err;
        input_set_capability(pad->dev, EV_FF, FF_RUMBLE);
        ff_err=input_ff_create_memless(pad->dev, NULL, mk_ff);
        if(ff_err){
            printk("mk_arcade_joystick_rpi: Failed to create force feedback device : %d",ff_err);
            ff_enable=false;
            ff_pwm_enable=false;
        }else{
            printk("mk_arcade_joystick_rpi: Force feedback device created");
            if(ff_enable){
                printk("mk_arcade_joystick_rpi: Force feedback : Strong GPIO pin : %d\n", ff_gpio_strong_pin);
                if(ff_effect_strong_reverse){printk("mk_arcade_joystick_rpi: Force feedback : Strong GPIO pin use reverse logic\n");}
                setGpioAsInput(ff_gpio_strong_pin); setGpioAsOuput(ff_gpio_strong_pin); //set pin as output, need to be set as input first
                if(ff_gpio_weak_pin!=-1){
                    printk("mk_arcade_joystick_rpi: Force feedback : Weak GPIO pin : %d\n", ff_gpio_weak_pin);
                    if(ff_effect_weak_reverse){printk("mk_arcade_joystick_rpi: Force feedback : Weak GPIO pin use reverse logic\n");}
                    setGpioAsInput(ff_gpio_weak_pin); setGpioAsOuput(ff_gpio_weak_pin); //set pin as output, need to be set as input first
                }
                
                if(ff_dir_enable){ //direction
                    printk("mk_arcade_joystick_rpi: Force feedback : Direction GPIO pin : %d\n", ff_gpio_dir_pin);
                    if(ff_gpio_dir_reverse){printk("mk_arcade_joystick_rpi: Force feedback : Direction GPIO pin use reverse logic\n");}
                    setGpioAsInput(ff_gpio_dir_pin); setGpioAsOuput(ff_gpio_dir_pin); //set pin as output, need to be set as input first
                }
            }
        }
    }
    
    err = input_register_device(pad->dev);
    if(err){goto err_free_dev;}
    
    return 0;
    
    err_free_dev: input_free_device(pad->dev); pad->dev = NULL; return err;
}


static struct mk __init *mk_probe(int *pads, int n_pads){
    struct mk *mk;
    int i;
    int count = 0;
    int err;
    
    mk = kzalloc(sizeof (struct mk), GFP_KERNEL);
    if(!mk){
        pr_err("Not enough memory\n");
        err = -ENOMEM;
        goto err_out;
    }
    
    mutex_init(&mk->mutex);
    //setup_timer(&mk->timer, mk_timer, (long) mk);
    g_mk = mk;
    INIT_DELAYED_WORK(&mk_delayed_work, mk_work_handler);
    
    for(i = 0; i < n_pads && i < MK_MAX_DEVICES; i++){
        if(!pads[i]){continue;}
        err = mk_setup_pad(mk, i, pads[i]);
        if(err){goto err_unreg_devs;}
        count++;
    }
    
    if(count == 0){
        pr_err("No valid devices specified\n");
        err = -EINVAL;
        goto err_free_mk;
    }
    
    return mk;
    
    err_unreg_devs: while(--i >= 0){if(mk->pads[i].dev){input_unregister_device(mk->pads[i].dev);}}
    err_free_mk: kfree(mk);
    err_out: return ERR_PTR(err);
}


static void mk_remove(struct mk *mk){
    int i;
    
    for (i = 0; i < MK_MAX_DEVICES; i++){
        if(mk->pads[i].dev){
            input_unregister_device(mk->pads[i].dev);
        }
    }
    
    kfree(mk);
}


static int __init mk_init(void){
    pr_err("Freeplay Button Driver\n");
    
    /* Set up gpio pointer for direct register access */
    if((gpio = ioremap(GPIO_BASE, 0xB0)) == NULL){
        pr_err("io remap failed\n");
        return -EBUSY;
    }
    
    if(debug_config_cfg.nargs > 0){ //if hkmode was not defined
        if(debug_config_cfg.debug[0]>0){debug_mode=abs(debug_config_cfg.debug[0]);} //enable debug mode
    }
    
    if(hkmode_cfg.nargs == 0){ //if hkmode was not defined
        hkmode_cfg.mode[0] = HOTKEY_MODE_TOGGLE; //default to HOTKEY_MODE_TOGGLE if not set
    }
    
    if(i2cbus_cfg.nargs == 0){ //if i2cbus addr was not defined
        i2cbus_cfg.busnum[0] = -1; //default to not using i2c
    }
    
    if(ads1015_cfg.nargs == 0){ //nns: if ads1015 addr was not defined
        ads1015_cfg.address[0] = 0; //default to not using it
    }
    
    if(analog_x1_cfg.nargs == 0){ //if analog input i2c addr was not defined
        analog_x1_cfg.address[0] = -1; //default to not using it, nns: -1 to avoid using it if ads1015 used
    }
    
    if(analog_y1_cfg.nargs == 0){ //if analog input i2c addr was not defined
        analog_y1_cfg.address[0] = -1; //default to not using it, nns: -1 to avoid using it if ads1015 used
    }
    
    if(analog_x2_cfg.nargs == 0){ //if analog input i2c addr was not defined
        analog_x2_cfg.address[0] = -1; //default to not using it, nns: -1 to avoid using it if ads1015 used
    }
    
    if(analog_y2_cfg.nargs == 0){ //if analog input i2c addr was not defined
        analog_y2_cfg.address[0] = -1; //default to not using it, nns: -1 to avoid using it if ads1015 used
    }
    
    if(auto_center_cfg.nargs > 0){ //if auto_center_analog set
        if(auto_center_cfg.auto_center[0]>0){auto_center = true;} //nns: if value > 0, auto center enable
    }
    
    if(analog_x1_direction_cfg.nargs > 0){ //if x1dir set
        if(analog_x1_direction_cfg.dir[0]<0){x1_reverse=true;} //nns:direction reversed
    }
    
    if(analog_y1_direction_cfg.nargs > 0){ //if y1dir set
        if(analog_y1_direction_cfg.dir[0]<0){y1_reverse=true;} //nns:direction reversed
    }
    
    if(analog_x2_direction_cfg.nargs > 0){ //if x2dir set
        if(analog_x2_direction_cfg.dir[0]<0){x2_reverse=true;} //nns:direction reversed
    }
    
    if(analog_y2_direction_cfg.nargs > 0){ //if y2dir set
        if(analog_y2_direction_cfg.dir[0]<0){y2_reverse=true;} //nns:direction reversed
    }
    
    if(ff_cfg.nargs > 0){ //nns: force feedback support
        ff_enable=true;
        ff_gpio_strong_pin=abs(ff_cfg.pins[0]); //ff strong pin
        if(ff_cfg.pins[0]<0){ff_effect_strong_reverse=true;} //ff strong reverse logic
        if(ff_cfg.nargs > 1){
            ff_gpio_weak_pin=abs(ff_cfg.pins[1]); //ff weak pin
            if(ff_cfg.pins[1]<0){ff_effect_weak_reverse=true;} //ff weak reverse logic
        }
    }
    
    if(ffpwm_cfg.nargs > 1){ //nns: pwm force feedback support
        ff_pwm_enable=true;
        ff_strong_pwm=abs(ffpwm_cfg.params[1]); //ff strong pwm output
        if(ffpwm_cfg.params[1]<0){ff_strong_pwm_reverse=true;} //ff strong pwm reverse logic
        if(ffpwm_cfg.nargs > 2){
            ff_weak_pwm=abs(ffpwm_cfg.params[2]); //ff weak pwm output
            if(ffpwm_cfg.params[2]<0){ff_weak_pwm_reverse=true;} //ff weak pwm reverse logic
        }
    }
    
    if(ffdir_cfg.nargs > 0){ //nns: force feedback direction support
        ff_dir_enable=true;
        ff_gpio_dir_pin=abs(ffdir_cfg.pins[0]); //ff direction pin
        if(ffdir_cfg.pins[0]<0){ff_gpio_dir_reverse=true;} //ff direction reverse logic
    }
    
    x1_analog_abs_params.min = ABS_PARAMS_DEFAULT_X_MIN; //x1 analog default min value for input_set_abs_params
    x1_analog_abs_params.max = ABS_PARAMS_DEFAULT_X_MAX; //x1 analog default max value for input_set_abs_params
    x1_analog_abs_params.fuzz = ABS_PARAMS_DEFAULT_X_FUZZ; //x1 analog default fuzz value for input_set_abs_params
    x1_analog_abs_params.flat = ABS_PARAMS_DEFAULT_X_FLAT; //x1 analog default flat value for input_set_abs_params
    if(analog_x1_abs_params_cfg.nargs > 0){x1_analog_abs_params.min = analog_x1_abs_params_cfg.abs_params[0]; //if analog_x1_abs_params_cfg set, set min value
        if(analog_x1_abs_params_cfg.nargs > 1){x1_analog_abs_params.max = analog_x1_abs_params_cfg.abs_params[1]; //set max value
            if(analog_x1_abs_params_cfg.nargs > 2){x1_analog_abs_params.fuzz = analog_x1_abs_params_cfg.abs_params[2]; //set fuzz value
                if(analog_x1_abs_params_cfg.nargs > 3){x1_analog_abs_params.flat = analog_x1_abs_params_cfg.abs_params[3];} //set flat value
            }
        }
    }
    
    y1_analog_abs_params.min = ABS_PARAMS_DEFAULT_Y_MIN; //y1 analog default min value for input_set_abs_params
    y1_analog_abs_params.max = ABS_PARAMS_DEFAULT_Y_MAX; //y1 analog default max value for input_set_abs_params
    y1_analog_abs_params.fuzz = ABS_PARAMS_DEFAULT_Y_FUZZ; //y1 analog default fuzz value for input_set_abs_params
    y1_analog_abs_params.flat = ABS_PARAMS_DEFAULT_Y_FLAT; //y1 analog default flat value for input_set_abs_params
    if(analog_y1_abs_params_cfg.nargs > 0){y1_analog_abs_params.min = analog_y1_abs_params_cfg.abs_params[0]; //if analog_y1_abs_params_cfg set, set min value
        if(analog_y1_abs_params_cfg.nargs > 1){y1_analog_abs_params.max = analog_y1_abs_params_cfg.abs_params[1]; //set max value
            if(analog_y1_abs_params_cfg.nargs > 2){y1_analog_abs_params.fuzz = analog_y1_abs_params_cfg.abs_params[2]; //set fuzz value
                if(analog_y1_abs_params_cfg.nargs > 3){y1_analog_abs_params.flat = analog_y1_abs_params_cfg.abs_params[3];} //set flat value
            }
        }
    }
    
    x2_analog_abs_params.min = ABS_PARAMS_DEFAULT_X_MIN; //x2 analog default min value for input_set_abs_params
    x2_analog_abs_params.max = ABS_PARAMS_DEFAULT_X_MAX; //x2 analog default max value for input_set_abs_params
    x2_analog_abs_params.fuzz = ABS_PARAMS_DEFAULT_X_FUZZ; //x2 analog default fuzz value for input_set_abs_params
    x2_analog_abs_params.flat = ABS_PARAMS_DEFAULT_X_FLAT; //x2 analog default flat value for input_set_abs_params
    if(analog_x2_abs_params_cfg.nargs > 0){x2_analog_abs_params.min = analog_x2_abs_params_cfg.abs_params[0]; //if analog_x2_abs_params_cfg set, set min value
        if(analog_x2_abs_params_cfg.nargs > 1){x2_analog_abs_params.max = analog_x2_abs_params_cfg.abs_params[1]; //set max value
            if(analog_x2_abs_params_cfg.nargs > 2){x2_analog_abs_params.fuzz = analog_x2_abs_params_cfg.abs_params[2]; //set fuzz value
                if(analog_x2_abs_params_cfg.nargs > 3){x2_analog_abs_params.flat = analog_x2_abs_params_cfg.abs_params[3];} //set flat value
            }
        }
    }
    
    y2_analog_abs_params.min = ABS_PARAMS_DEFAULT_Y_MIN; //y2 analog default min value for input_set_abs_params
    y2_analog_abs_params.max = ABS_PARAMS_DEFAULT_Y_MAX; //y2 analog default max value for input_set_abs_params
    y2_analog_abs_params.fuzz = ABS_PARAMS_DEFAULT_Y_FUZZ; //y2 analog default fuzz value for input_set_abs_params
    y2_analog_abs_params.flat = ABS_PARAMS_DEFAULT_Y_FLAT; //y2 analog default flat value for input_set_abs_params
    if(analog_y2_abs_params_cfg.nargs > 0){y2_analog_abs_params.min = analog_y2_abs_params_cfg.abs_params[0]; //if analog_y2_abs_params_cfg set, set min value
        if(analog_y2_abs_params_cfg.nargs > 1){y2_analog_abs_params.max = analog_y2_abs_params_cfg.abs_params[1]; //set max value
            if(analog_y2_abs_params_cfg.nargs > 2){y2_analog_abs_params.fuzz = analog_y2_abs_params_cfg.abs_params[2]; //set fuzz value
                if(analog_y2_abs_params_cfg.nargs > 3){y2_analog_abs_params.flat = analog_y2_abs_params_cfg.abs_params[3];} //set flat value
            }
        }
    }
    
    
    if(i2cbus_cfg.busnum[0] >= 0){
        int rm;
        int retry;
        rm = request_module("i2c_bcm2835");
        printk("mk_arcade_joystick_rpi: request_module i2c_bcm2835 returned %d\n", rm);
        
        i2c_dev = i2c_get_adapter(i2cbus_cfg.busnum[0]);
        
        for(retry=0;retry<3;retry++){
            if(!i2c_dev){
                printk("mk_arcade_joystick_rpi: I2C bus %d NOT opened (sleeping and retrying)\n", i2cbus_cfg.busnum[0]);
                msleep(500);
                i2c_dev = i2c_get_adapter(i2cbus_cfg.busnum[0]);
            }else{
                retry=3;
            }
        }
        
        if(i2c_dev){
            int16_t value = 0;
            i2c_dev->timeout=1; //try to set i2c timeout to 10ms (1=10ms)
            
            //https://github.com/torvalds/linux/blob/master/drivers/i2c/i2c-dev.c:482
            printk("mk_arcade_joystick_rpi: I2C bus %d opened\n", i2cbus_cfg.busnum[0]);
            printk("mk_arcade_joystick_rpi: I2C bus timeout set to %d ms\n", (i2c_dev->timeout)*10);
            
            if(ads1015_cfg.address[0] > 0){ //nns: add ads1015 support
                i2c_client_x1 = i2c_new_ADS1015(i2c_dev, ads1015_cfg.address[0]);
                if(i2c_client_x1){ads1015_enable=true;}
            }
            
            if(auto_center){printk("mk_arcade_joystick_rpi: Analog auto center enable\n");
            }else{printk("mk_arcade_joystick_rpi: Analog auto center disable\n");}
            
            if(!ads1015_enable&&ads1015_cfg.address[0]==0){ //use MCP3021
                if(analog_x1_cfg.address[0] > 0){
                    i2c_client_x1 = i2c_new_MCP3021(i2c_dev, analog_x1_cfg.address[0]);
                    if(i2c_client_x1){
                        printk("mk_arcade_joystick_rpi: X1 assigned to I2C address 0x%02X\n", analog_x1_cfg.address[0]);
                        value = i2c_smbus_read_word_swapped(i2c_client_x1, 0);
                        if(value & 0x8000){
                            printk("mk_arcade_joystick_rpi: X1 chip not found\n");
                            i2c_unregister_device(i2c_client_x1);
                            i2c_client_x1 = NULL;
                        }else{
                            if(x1_reverse){ //nns: reverse direction
                                printk("mk_arcade_joystick_rpi: X1 direction reversed\n");
                                value = 4096-value; //nns: reverse 12bits value
                            }
                            
                            if(value >= 0){x1_offset = value-2047;} //nns: center offset
                            printk("mk_arcade_joystick_rpi: X1 initial value : %d (0x%04X)\n", value, value);
                            x1_enable = true;
                        }
                    }
                }
                
                if(analog_y1_cfg.address[0] > 0){
                    i2c_client_y1 = i2c_new_MCP3021(i2c_dev, analog_y1_cfg.address[0]);
                    if(i2c_client_y1){
                        printk("mk_arcade_joystick_rpi: Y1 assigned to I2C address 0x%02X\n", analog_y1_cfg.address[0]);
                        value = i2c_smbus_read_word_swapped(i2c_client_y1, 0);
                        if(value & 0x8000){
                            printk("mk_arcade_joystick_rpi: Y1 chip not found\n");
                            i2c_unregister_device(i2c_client_y1);
                            i2c_client_y1 = NULL;
                        }else{
                            if(y1_reverse){ //nns: reverse direction
                                printk("mk_arcade_joystick_rpi: Y1 direction reversed\n");
                                value = 4096-value; //nns: reverse 12bits value
                            }
                            
                            if(value >= 0){y1_offset = value-2047;} //nns: center offset
                            printk("mk_arcade_joystick_rpi: Y1 initial value : %d (0x%04X)\n", value, value);
                            y1_enable = true;
                        }
                    }
                }
                
                if(analog_x2_cfg.address[0] > 0){
                    i2c_client_x2 = i2c_new_MCP3021(i2c_dev, analog_x2_cfg.address[0]);
                    if(i2c_client_x2){
                        printk("mk_arcade_joystick_rpi: X2 assigned to I2C address 0x%02X\n", analog_x2_cfg.address[0]);
                        value = i2c_smbus_read_word_swapped(i2c_client_x2, 0);
                        if(value & 0x8000){
                            printk("mk_arcade_joystick_rpi: X2 chip not found\n");
                            i2c_unregister_device(i2c_client_x2);
                            i2c_client_x2 = NULL;
                        }else{
                            if(x2_reverse){ //nns: reverse direction
                                printk("mk_arcade_joystick_rpi: X2 direction reversed\n");
                                value = 4096-value; //nns: reverse 12bits value
                            }
                            
                            if(value >= 0){x2_offset = value-2047;} //nns: center offset
                            printk("mk_arcade_joystick_rpi: X2 initial value : %d (0x%04X)\n", value, value);
                            x2_enable = true;
                        }
                    }
                }
                
                if(analog_y2_cfg.address[0] > 0){
                    i2c_client_y2 = i2c_new_MCP3021(i2c_dev, analog_y2_cfg.address[0]);
                    if(i2c_client_y2){
                        printk("mk_arcade_joystick_rpi: Y2 assigned to I2C address 0x%02X\n", analog_y2_cfg.address[0]);
                        value = i2c_smbus_read_word_swapped(i2c_client_y2, 0);
                        if(value & 0x8000){
                            printk("mk_arcade_joystick_rpi: Y2 chip not found\n");
                            i2c_unregister_device(i2c_client_y2);
                            i2c_client_y2 = NULL;
                        }else{
                            if(y2_reverse){ //nns: reverse direction
                                printk("mk_arcade_joystick_rpi: Y2 direction reversed\n");
                                value = 4096-value; //nns: reverse 12bits value
                            }
                            
                            if(value >= 0){y2_offset = value-2047;} //nns: center offset
                            printk("mk_arcade_joystick_rpi: Y2 initial value : %d (0x%04X)\n", value, value);
                            y2_enable = true;
                        }
                    }
                }
            }else if(ads1015_enable){ //use ads1015
                printk("mk_arcade_joystick_rpi: ADS1015 I2C address 0x%02X\n", ads1015_cfg.address[0]);
                
                if(analog_x1_cfg.address[0] >= 0 && analog_x1_cfg.address[0] < 4){
                    ads1015_lookup[0]=analog_x1_cfg.address[0];
                    value = ADS1015_read(i2c_client_x1,0);
                    if(value>=0){
                        printk("mk_arcade_joystick_rpi: X1 assigned to AIN%d\n", analog_x1_cfg.address[0]);
                        if(x1_reverse){ //nns: reverse direction
                            printk("mk_arcade_joystick_rpi: X1 direction reversed\n");
                            value = 4096-value; //nns: reverse 12bits value
                        }
                        x1_offset = value-2047; //nns: center offset
                        printk("mk_arcade_joystick_rpi: X1 initial value : %d (0x%04X)\n", value, value);
                        x1_enable = true;
                    }else{
                        printk("mk_arcade_joystick_rpi: X1 failed, disabled\n");
                        if(debug_mode>0){printk("mk_arcade_joystick_rpi: DEBUG : returned %i\n",value);}
                        ads1015_lookup[0]=-1;
                    }
                }
                
                if(analog_y1_cfg.address[0] >= 0 && analog_y1_cfg.address[0] < 4){
                    ads1015_lookup[1]=analog_y1_cfg.address[0];
                    value = ADS1015_read(i2c_client_x1,1);
                    if(value>=0){
                        printk("mk_arcade_joystick_rpi: Y1 assigned to AIN%d\n", analog_y1_cfg.address[0]);
                        if(y1_reverse){ //nns: reverse direction
                            printk("mk_arcade_joystick_rpi: Y1 direction reversed\n");
                            value = 4096-value; //nns: reverse 12bits value
                        }
                        y1_offset = value-2047; //nns: center offset
                        printk("mk_arcade_joystick_rpi: Y1 initial value : %d (0x%04X)\n", value, value);
                        y1_enable = true;
                    }else{
                        printk("mk_arcade_joystick_rpi: Y1 failed, disabled\n");
                        if(debug_mode>0){printk("mk_arcade_joystick_rpi: returned %i\n",value);}
                        ads1015_lookup[1]=-1;
                    }
                }
                
                if(analog_x2_cfg.address[0] >= 0 && analog_x2_cfg.address[0] < 4){
                    ads1015_lookup[2]=analog_x2_cfg.address[0];
                    value = ADS1015_read(i2c_client_x1,2);
                    if(value>=0){
                        printk("mk_arcade_joystick_rpi: X2 assigned to AIN%d\n", analog_x2_cfg.address[0]);
                        if(x2_reverse){ //nns: reverse direction
                            printk("mk_arcade_joystick_rpi: X2 direction reversed\n");
                            value = 4096-value; //nns: reverse 12bits value
                        }
                        x2_offset = value-2047; //nns: center offset
                        printk("mk_arcade_joystick_rpi: X2 initial value : %d (0x%04X)\n", value, value);
                        x2_enable = true;
                    }else{
                        printk("mk_arcade_joystick_rpi: X2 failed, disabled\n");
                        if(debug_mode>0){printk("mk_arcade_joystick_rpi: returned %i\n",value);}
                        ads1015_lookup[2]=-1;
                    }
                }
                
                if(analog_y2_cfg.address[0] >= 0 && analog_y2_cfg.address[0] < 4){
                    ads1015_lookup[3]=analog_y2_cfg.address[0];
                    value = ADS1015_read(i2c_client_x1,3);
                    if(value>=0){
                        printk("mk_arcade_joystick_rpi: Y2 assigned to AIN%d\n", analog_y2_cfg.address[0]);
                        if(y2_reverse){ //nns: reverse direction
                            printk("mk_arcade_joystick_rpi: Y2 direction reversed\n");
                            value = 4096-value; //nns: reverse 12bits value
                        }
                        y2_offset = value-2047; //nns: center offset
                        printk("mk_arcade_joystick_rpi: Y2 initial value : %d (0x%04X)\n", value, value);
                        y2_enable = true;
                    }else{
                        printk("mk_arcade_joystick_rpi: Y2 failed, disabled\n");
                        if(debug_mode>0){printk("mk_arcade_joystick_rpi: returned %i\n",value);}
                        ads1015_lookup[3]=-1;
                    }
                }
            }
            
            if(!auto_center){ //nns: if auto center disable, reset all offset
                if(x1_enable){x1_offset=(((x1_analog_abs_params.max-x1_analog_abs_params.min)/2)+x1_analog_abs_params.min)-2047;} //nns: compute offset based on min and max
                if(y1_enable){y1_offset=(((y1_analog_abs_params.max-y1_analog_abs_params.min)/2)+y1_analog_abs_params.min)-2047;} //nns: compute offset based on min and max
                if(x2_enable){x2_offset=(((x2_analog_abs_params.max-x2_analog_abs_params.min)/2)+x2_analog_abs_params.min)-2047;} //nns: compute offset based on min and max
                if(y2_enable){y2_offset=(((y2_analog_abs_params.max-y2_analog_abs_params.min)/2)+y2_analog_abs_params.min)-2047;} //nns: compute offset based on min and max
            }
            
            if(x1_enable){printk("mk_arcade_joystick_rpi: X1 offset : %d\n", x1_offset);}
            if(y1_enable){printk("mk_arcade_joystick_rpi: Y1 offset : %d\n", y1_offset);}
            if(x2_enable){printk("mk_arcade_joystick_rpi: X2 offset : %d\n", x2_offset);}
            if(y2_enable){printk("mk_arcade_joystick_rpi: Y2 offset : %d\n", y2_offset);}
            
            if(ff_pwm_enable){ //nns: add PCA9633 support for force feedback
                pca9633_client = i2c_new_PCA9633(i2c_dev, ffpwm_cfg.params[0]);
                if(pca9633_client){
                    printk("mk_arcade_joystick_rpi: PCA9633 assigned to I2C address 0x%02X\n", ffpwm_cfg.params[0]);
                    value = i2c_smbus_read_word_swapped(pca9633_client, 0);
                    if(value & 0x8000){
                        printk("mk_arcade_joystick_rpi: PCA9633 chip not found\n");
                        i2c_unregister_device(pca9633_client);
                        pca9633_client = NULL;
                        ff_pwm_enable=false;
                    }else{
                        pca9633_ledout=i2c_smbus_read_byte_data(pca9633_client,(uint8_t)0x08); //read ledout register
                        pca9633_ledout_backup=pca9633_ledout; //backup ledout value to restore when killing driver
                        
                        if(ff_strong_pwm!=-1){
                            pca9633_ledout=pca9633_ledout|(0x02<<(ff_strong_pwm*2)); //ledout to pwm for this output
                            if(ff_strong_pwm_reverse){
                                printk("mk_arcade_joystick_rpi: Strong PWM output : %d (reversed logic)\n", ff_strong_pwm);
                                i2c_smbus_write_byte_data(pca9633_client,(uint8_t)(ff_strong_pwm+2),(uint8_t)0xFF); //send pwm to i2c, reverse logic, set 0xFF
                            }else{
                                printk("mk_arcade_joystick_rpi: Strong PWM output : %d\n", ff_strong_pwm);
                                i2c_smbus_write_byte_data(pca9633_client,(uint8_t)(ff_strong_pwm+2),(uint8_t)0x00); //send pwm to i2c, set 0x00
                            }
                        }
                        if(ff_weak_pwm!=-1){
                            pca9633_ledout=pca9633_ledout|(0x02<<(ff_weak_pwm*2)); //ledout to pwm for this output
                            if(ff_weak_pwm_reverse){
                                printk("mk_arcade_joystick_rpi: Weak PWM output : %d (reversed logic)\n", ff_weak_pwm);
                                i2c_smbus_write_byte_data(pca9633_client,(uint8_t)(ff_weak_pwm+2),(uint8_t)0xFF); //send pwm to i2c, reverse logic, set 0xFF
                            }else{
                                printk("mk_arcade_joystick_rpi: Weak PWM output : %d\n", ff_weak_pwm);
                                i2c_smbus_write_byte_data(pca9633_client,(uint8_t)(ff_weak_pwm+2),(uint8_t)0x00); //send pwm to i2c, set 0x00
                            }
                        }
                        
                        i2c_smbus_write_byte_data(pca9633_client,(uint8_t)0x08,(uint8_t)pca9633_ledout); //send ledout to i2c
                        printk("mk_arcade_joystick_rpi: PCA9633 LEDOUT : 0x%02X (initial), 0x%02X (new)\n",pca9633_ledout_backup,pca9633_ledout);
                    }
                }else{
                    printk("mk_arcade_joystick_rpi: PCA9633 : Failed to assign I2C address 0x%02X\n", ffpwm_cfg.params[0]);
                    ff_pwm_enable=false;
                }
            }
        }else{
            printk("mk_arcade_joystick_rpi ERROR: I2C bus %d NOT opened (make sure that I2C is enabled and loaded before this driver)\n", i2cbus_cfg.busnum[0]);
        }
    }else{
        ff_pwm_enable=false; //nns: disable pwm force feedback is no I2C bus set
    }
    
    
    if(mk_cfg.nargs < 1){
        pr_err("at least one device must be specified\n");
        return -EINVAL;
    }else{
        mk_base = mk_probe(mk_cfg.args, mk_cfg.nargs); //jump
        if(IS_ERR(mk_base)){return -ENODEV;}
    }
    
    return 0;
}

static void __exit mk_exit(void){
    if(mk_base){mk_remove(mk_base);}
    
    printk("mk_arcade_joystick_rpi: Exiting\n");
    
    if(ads1015_enable && i2c_client_x1 != NULL){i2c_unregister_device(i2c_client_x1);} //nns: add ads1015 support
    
    if(x1_enable){
        if(!ads1015_enable && i2c_client_x1 != NULL){i2c_unregister_device(i2c_client_x1);}
        printk("mk_arcade_joystick_rpi: X1 limits : min: %d (0x%04X), max: %d (0x%04X) : x1params=%d,%d,%d,%d\n", x1_min, x1_min, x1_max, x1_max   ,x1_min,x1_max,x1_analog_abs_params.fuzz,x1_analog_abs_params.flat); //nns: add config format
    }
    
    if(y1_enable){
        if(!ads1015_enable && i2c_client_y1 != NULL){i2c_unregister_device(i2c_client_y1);}
        printk("mk_arcade_joystick_rpi: Y1 limits : min: %d (0x%04X), max: %d (0x%04X) : y1params=%d,%d,%d,%d\n", y1_min, y1_min, y1_max, y1_max   ,y1_min,y1_max,y1_analog_abs_params.fuzz,y1_analog_abs_params.flat); //nns: add config format
    }
    
    if(x2_enable){
        if(!ads1015_enable && i2c_client_x2 != NULL){i2c_unregister_device(i2c_client_x2);}
        printk("mk_arcade_joystick_rpi: X2 limits : min: %d (0x%04X), max: %d (0x%04X) : x2params=%d,%d,%d,%d\n", x2_min, x2_min, x2_max, x2_max   ,x2_min,x2_max,x2_analog_abs_params.fuzz,x2_analog_abs_params.flat); //nns: add config format
    }
    
    if(y2_enable){
        if(!ads1015_enable && i2c_client_y2 != NULL){i2c_unregister_device(i2c_client_y2);}
        printk("mk_arcade_joystick_rpi: Y2 limits : min: %d (0x%04X), max: %d (0x%04X) : y2params=%d,%d,%d,%d\n", y2_min, y2_min, y2_max, y2_max   ,y2_min,y2_max,y2_analog_abs_params.fuzz,y2_analog_abs_params.flat); //nns: add config format
    }
    
    //nns: force feedback
    if(ff_enable){
        if(ff_gpio_strong_pin!=-1){ //gpio strong
            if(ff_effect_strong_reverse){GpioOuputSet(ff_gpio_strong_pin); //reverse logic, set high
            }else{GpioOuputClr(ff_gpio_strong_pin);} //set low
        }
        if(ff_gpio_weak_pin!=-1){ //gpio weak
            if(ff_effect_weak_reverse){GpioOuputSet(ff_gpio_weak_pin); //reverse logic, set high
            }else{GpioOuputClr(ff_gpio_weak_pin);} //set low
        }
    }
    
    if(ff_dir_enable){
        if(ff_gpio_dir_pin!=-1){ //gpio dir
            if(ff_gpio_dir_reverse){GpioOuputSet(ff_gpio_dir_pin); //reverse logic, set high
            }else{GpioOuputClr(ff_gpio_dir_pin);} //set low
        }
    }
    
    if(ff_pwm_enable && pca9633_client != NULL){ //nns: add PCA9633 support
        if(ff_strong_pwm!=-1){ //pwm strong
            if(ff_strong_pwm_reverse){i2c_smbus_write_byte_data(pca9633_client,(uint8_t)(ff_strong_pwm+2),(uint8_t)0xFF); //send pwm to i2c, reverse logic, set 0xFF
            }else{i2c_smbus_write_byte_data(pca9633_client,(uint8_t)(ff_strong_pwm+2),(uint8_t)0x00);} //send pwm to i2c, set 0x00
        }
        if(ff_weak_pwm!=-1){ //pwm weak
            if(ff_weak_pwm_reverse){i2c_smbus_write_byte_data(pca9633_client,(uint8_t)(ff_weak_pwm+2),(uint8_t)0xFF); //send pwm to i2c, reverse logic, set 0xFF
            }else{i2c_smbus_write_byte_data(pca9633_client,(uint8_t)(ff_weak_pwm+2),(uint8_t)0x00);} //send pwm to i2c, set 0x00
        }
        i2c_smbus_write_byte_data(pca9633_client,(uint8_t)0x08,(uint8_t)pca9633_ledout_backup); //send ledout to i2c
        i2c_unregister_device(pca9633_client);
        printk("mk_arcade_joystick_rpi: PCA9633 LEDOUT restored : 0x%02X\n",pca9633_ledout_backup);
    }
    
    iounmap(gpio);
}

module_init(mk_init);
module_exit(mk_exit);
