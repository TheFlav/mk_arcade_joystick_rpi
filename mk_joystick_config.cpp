/*
NNS @ 2019
mk_joystick_config
Allow user to create new configuration file for mk_arcade_joystick_rpi Analog/GPIO controller driver.
*/
const char programversion[]="0.1c"; //program version

#include <fcntl.h> //file io
#include <stdio.h> //stream io
#include <stdlib.h> //standard
#include <unistd.h> //standard
#include <cstring> //string
#include <limits.h> //limits
#include <errno.h> //errno
#include <linux/i2c-dev.h> //i2c library
#include <sys/ioctl.h> //sys io
#include <pthread.h> //pthread
#include <wiringPi.h> //wiringpi
#include <sys/time.h> //time
#include <time.h> //time
#include <sys/stat.h> //file stat



//GPIO variables
pthread_t gpio_thread; //gpio thread id
int gpio_thread_rc=-1; //gpio thread return code
//int gpio_input_trigger=-1;
int gpio_input[55]; //store if gpio pin is input
int gpio_input_activelow[55]; //store if gpio pin is active low
long long gpio_input_timestamp[55]; //store if gpio pin triggered timestamp
bool gpio_input_enable[55]; //if specific gpio pin allow to be monitored
int button_A_gpio=-1; //gpio button A for user interaction
int button_B_gpio=-1; //gpio button B for user interaction
int button_hkmode=1; //hotkey mode: 1=normal, 2=toggle
int button_tmp_gpio=-1; //used for user interaction
int button_tmpbis_gpio=-1; //used for user interaction, most likely for validation
int button_pressed=-1; //last valid gpio pin triggered
int button_pressed_tmp=-1; //backup previous variable
int button_table[]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}; //store user input
int button_table_logic[]={1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}; //store user input reverse logic
int gpio_button_link_table[]={12,0,1,2,3,6,7,10,9,4,5,11,8,13,14,15,16,17,18,19,20}; //used to match button name and config


//I2C/ADC variables
pthread_t adc_thread; //adc thread id
int adc_thread_rc=-1; //adc thread return code
char i2c_bus[]="/dev/i2c-1"; //path to i2c bus
int i2c_handle; //handle to get i2c data
unsigned char i2c_buffer[16] = {0}; //read/write buffer array
bool adc_mcp3021 = false; //is mcp3021 enable
bool adc_ads1015 = false; //is ads1015 enable
bool adc_user = true; //user selected the adc chip type
bool adc_calibration = false; //adc calibration started
bool adc_calibration_done = false; //adc calibration done
int adc_address[] = {-1,-1,-1,-1,-1,-1,-1,-1}; //detected i2c chip adresses, -1 if nothing
int adc_address_ads1015 = -1; //specific to ads1015 chip
int adc_address_count=0; //count of found i2c chip adress
int adc_detected = 0; //numbers of i2c chip detected
int adc_detected_back = 0; //numbers of i2c chip detected
int adc_mapping[] = {-1,-1,-1,-1}; //corresponding array to adc_address: x1,y1,x2,y2. -1 if nothing
int adc_center[] = {-1,-1,-1,-1,-1,-1,-1,-1}; //store center value, used to detect noise and right adc-axis correspondance
int adc_value[] = {-1,-1,-1,-1,-1,-1,-1,-1}; //store adc value
int adc_min[] = {-1,-1,-1,-1,-1,-1,-1,-1}; //store adc min value
int adc_max[] = {-1,-1,-1,-1,-1,-1,-1,-1}; //store adc max value
int adc_reverse[] = {1,1,1,1,1,1,1,1}; //store reverse value: -1=reverse, 1=not reverse
int adc_skip[] = {1,1,1,1,1,1,1,1}; //use to avoid user to use same adc chip for 2 axis
int adc_skipbackup[] = {1,1,1,1,1,1,1,1}; //backup of the previous variable
int adc_tmp = 0; //backup of the axis reversal
long adc_delta[] = {0,0,0,0,0,0,0,0}; //use to detect adc-axis correspondance
int adc_maxnoise=60; //use to detect if nothing connected ot input pin
int adc_val_tmp=0; //store current adc value
const char ads1015_config_ain[]={0x40,0x50,0x60,0x70}; //ads1015:ain use to create config
bool adc_autocenter=false; //allow use to enable auto analog center


//General variables
bool debug=false; //debug enable?
//struct timeval scanning_timeout;
long long scanning_start=0; //user for timeout stuff in second
bool retry=false; //allow use to change answer
bool retrymain=false; //allow use to change main answer
char text_config[4096]; //store config file
char text_config_buffer[1024]; //store config 'buffer'
bool save_config=false; //allow saving of new config file
char config_path[]="/etc/modprobe.d/mk_arcade_joystick.conf"; //full path to config
char config_backup_path[PATH_MAX]; //full path to backup
FILE *temp_filehandle; //file handle to get check if driver loaded


//Text variables
char str_analog_position[][7] = {"LEFT","LEFT","RIGHT","RIGHT"}; //analog stick position on the device
char str_analog_direction[][7] = {"Left","Up","Left","Up"}; //analog stick direction to push for axis detection
char str_analog_axis[][2] = {"X","Y","X","Y"}; //correspondance for axis to last variable
char str_button_name[][16] = {"Hotkey","Dpad Up","Dpad Down","Dpad Left","Dpad Right","A","B","X","Y","Start","Select","L1","R1","L2","R2","L3-C","R3-Z","RESERVED","RESERVED","RESERVED","RESERVED"}; //input name
//char str_button_name[][16] = {"Hotkey-12","Dpad Up-0","Dpad Down-1","Dpad Left-2","Dpad Right-3","A-6","B-7","X-10","Y-9","Start-4","Select-5","L1-11","R1-8","L2-13","R2-14","C-15","Z-16","RESERVED","RESERVED","RESERVED","RESERVED"}; //keep for debug


//Functions declaration, need when call function declare after the caller
void ADS1015_read(int addr);
void MCP3021_read(int addr,int index);
int adc_highest_delta();


long long timestamp_msec(){ //recover timestamp in msec, https://stackoverflow.com/questions/3756323/how-to-get-the-current-time-in-milliseconds-from-c-in-linux
	struct timeval te;gettimeofday(&te, NULL);long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000;return milliseconds;
}


void *gpio_routine(void *){ //GPIO input thread routine
	if(debug){printf("---Debug : GPIO : Thread (%lu) started\n",gpio_thread);} //debug
	// gpio_thread_rc values : 0:ok, -1:failed, -2:fail because of wiringPi
	
	if(wiringPiSetupGpio()==-1){printf("GPIO : wiringPi not initialized\n");gpio_thread_rc=-2;} //failed
	
	int gpio_pin; //current gpio pin
	for(int gpio_pin=0;gpio_pin<54;gpio_pin++){ //initial loop to fill empty array
		gpio_input[gpio_pin]=0; //default value
		if(getAlt(gpio_pin)==0){ //pin is a input
			gpio_input[gpio_pin]=0; //initial value
			gpio_input_enable[gpio_pin]=true; //used when user bind keys
			if(digitalRead(gpio_pin)){gpio_input_activelow[gpio_pin]=0;}else{gpio_input_activelow[gpio_pin]=1;} //assume low as activelow
			gpio_input_timestamp[gpio_pin]=0; //initialize timestamp
			if(gpio_pin!=7&&gpio_pin!=20&&gpio_pin!=21){pullUpDnControl(gpio_pin,PUD_UP);usleep(1000);} //pull all input high except of pin used for power slider, allow 1ms to set pullup register
		}
	}
	
	gpio_thread_rc=0; //update thread state
	
	while(gpio_thread_rc>-1){ //loop until fail
		for(int gpio_pin=0;gpio_pin<54;gpio_pin++){ //initial loop to fill array with real data
			if(getAlt(gpio_pin)==0){ //pin is a input
				if((!gpio_input_activelow[gpio_pin]&&!digitalRead(gpio_pin))||(gpio_input_activelow[gpio_pin]&&digitalRead(gpio_pin))){ //gpio button is pressed
					if(gpio_input_timestamp[gpio_pin]==0){ //no timestamp
						if(debug){printf("---Debug : GPIO : %d : %lli : %lli\n",gpio_pin,timestamp_msec(),gpio_input_timestamp[gpio_pin]);} //debug
						gpio_input_timestamp[gpio_pin]=timestamp_msec(); //update msec timestamp if no pressed before
					}else{
						if(gpio_input[gpio_pin]!=1&&button_pressed==-1&&(timestamp_msec()-gpio_input_timestamp[gpio_pin])>50){ //allow to trigger only once, trigger more than 50msec to avoid detect because of noise
							if(debug){printf("---Debug : GPIO : %d : %lli : %lli\n",gpio_pin,timestamp_msec(),gpio_input_timestamp[gpio_pin]);} //debug
							button_pressed=gpio_pin; //assign pin to pressed
							gpio_input[gpio_pin]=1; //update pin array
							break; //exit for loop
						}
					}
				}else{ //gpio button not pressed
					gpio_input[gpio_pin]=0; //reset pin value
					gpio_input_timestamp[gpio_pin]=0; //reset pin timestamp
				}
			}
		}
		
		usleep(20*1000); //wait 20msec
	}
	
	pthread_cancel(gpio_thread); //close input thread if trouble
	return NULL;
}



void *adc_routine(void *){ //ADC input thread routine
	if(debug){printf("---Debug : ADC : Thread (%lu) started\n",adc_thread);} //debug
	// gpio_thread_rc values : 0:ok, -1:scaning for adc, -2:adc detection failed, -3:i2c bus failed, 
	// detect i2c adress : mcp(72 to 79), ads(72 to 75)
	
	//start adc detection
	int loop; //use for loop
	if((i2c_handle=open(i2c_bus,O_RDWR))<0){ //failed open i2c bus
		printf("\033[91mFailed to open the I2C bus : %d, Analog detection skipped\033[0m\n",i2c_bus);
		adc_thread_rc=-3; //update thread state
	}else{ //no problem opening i2c bus
		for(loop=72;loop<=79;loop++){ //try to detect adc chip adresses
			if(ioctl(i2c_handle,I2C_SLAVE,loop)<0){ //try access i2c device
				if(debug){printf("---Debug: Failed to access I2C device : %d (0x%2x)\n",loop,loop&0xFF);} //debug
			}else{
				if(read(i2c_handle,i2c_buffer,2)!=2){ //fail to read data
					if(debug){printf("---Debug: Failed to read I2C device : %d (0x%2x)\n",loop,loop&0xFF);} //debug
				}else{ //success
					if(debug){printf("---Debug: I2C device detected : %d (0x%2x)\n",loop,loop&0xFF);} //debug
					adc_address[adc_detected]=loop; //update adc chip adress
					adc_detected++; //increase adc chip detected
					
					//check if ADS1015
					if(!adc_ads1015){
						i2c_buffer[0]=0x01;i2c_buffer[1]=0x00;i2c_buffer[2]=0x00; //reset config register
						write(i2c_handle,i2c_buffer,3);usleep(1000); //write register and wait 1msec
						read(i2c_handle,i2c_buffer,2); //get result
						adc_val_tmp=(i2c_buffer[0]<<8)|(i2c_buffer[1]&0xff); //combine to int
						if(debug){printf("---Debug: read : 0x%04x : ",adc_val_tmp);} //debug
						if(adc_val_tmp==0x0000){ //ads1015 detected
							if(debug){printf("ADS1015\n");} //debug
							adc_ads1015=true;
							adc_address_ads1015=loop; //update adc chip adress
							i2c_buffer[0]=0x01;i2c_buffer[1]=(0x83|ads1015_config_ain[0]);i2c_buffer[2]=(0xE3); //config register: AIN0, 3300SPS, +-4.096v FSR
							write(i2c_handle,i2c_buffer,3); //write config
						}else{
							if(debug){printf("MCP3021\n");} //debug
							adc_mcp3021=true;
						} //assume adc chip is mcp3021
					}
				}
			}
		}
		/*
		if(adc_detected==0){adc_thread_rc=-2; //no chip detected
		}else if(adc_detected==1){adc_ads1015=true;adc_thread_rc=0; //ads1015 use
		}else{adc_mcp3021=true;adc_thread_rc=0;} //mcp3021 use
		*/
		if(adc_detected==0){adc_thread_rc=-2; //no chip detected
		}else{adc_thread_rc=0;} //ok
		
	}
	
	while(adc_thread_rc>-1){ //loop until fail
		if(adc_user&&adc_calibration){ // user selected right adc type but calibration in progress
			if(adc_ads1015){ADS1015_read(adc_address_ads1015); //read ads1015 values
			}else if(adc_mcp3021){ //mcp3021 use
				for(loop=0;loop<adc_address_count;loop++){ //read loop
					if(adc_address[loop]!=-1){MCP3021_read(adc_address[loop],loop);} //read mcp3021 value
				}
			}
			
			if((time(NULL)-scanning_start)>4){adc_calibration_done=true; adc_calibration=false;} //calibration done
		}else if(adc_user&&adc_calibration_done){ // user selected right adc type and calibration done
			if(adc_ads1015){ADS1015_read(adc_address_ads1015); //read ads1015 values
			}else if(adc_mcp3021){ //mcp3021 use
				if(debug){printf("\r");} //debug
				for(loop=0;loop<adc_address_count;loop++){ //read loop
					if(adc_address[loop]!=-1){MCP3021_read(adc_address[loop],loop);} //read mcp3021 value
				}
			}
			
		}else{sleep(1);} // standby until user selected right adc type
	}
	
	if(adc_thread_rc>-3){close(i2c_handle);} //close the handle
	pthread_cancel(adc_thread); //close input thread if trouble
	return NULL;
}



void MCP3021_read(int addr,int index){ //function to read MCP3021 values
	if(addr<0){return ;} //no adress, fail
	if(ioctl(i2c_handle,I2C_SLAVE,addr)>=0){ //try access i2c device
		//if(debug){printf("\r");} //debug
		read(i2c_handle,i2c_buffer,2); //get result
		adc_val_tmp=(i2c_buffer[0]<<8)|(i2c_buffer[1]&0xff); //combine buffer bytes into integer
		adc_value[index]=adc_val_tmp; //backup new value
		if(adc_center[index]==-1){adc_center[index]=adc_val_tmp;} //set center value if not set
		if(adc_min[index]==-1){adc_min[index]=adc_val_tmp;} //set min value if not set
		if(adc_val_tmp<adc_min[index]){adc_min[index]=adc_val_tmp;} //update min value
		if(adc_max[index]==-1){adc_max[index]=adc_val_tmp;} //set max value if not set
		if(adc_val_tmp>adc_max[index]){adc_max[index]=adc_val_tmp;} //update max value
		if(debug){printf("%04d (min: %04d,center: %04d,max: %04d) , ",adc_val_tmp,adc_min[index],adc_center[index],adc_max[index]);} //debug
	}else{return ;}
}



void ADS1015_read(int addr){
	if(addr<0){return ;} //no adress, fail
	int loop; //use for loop
	if(ioctl(i2c_handle,I2C_SLAVE,addr)>=0){ //try access i2c device
		//if(debug){printf("\r");} //debug
		for(loop=0;loop<4;loop++){ //read loop
			i2c_buffer[0]=0x01; //config register
			i2c_buffer[1]=(0x83|ads1015_config_ain[loop]);i2c_buffer[2]=(0xE3); //3300SPS, +-4.096v FSR
			write(i2c_handle,i2c_buffer,3); //write config
			usleep(500); //wait for convertion to be done
			i2c_buffer[0]=0; //convertion register
			write(i2c_handle,i2c_buffer,1); //select conversion register
			read(i2c_handle,i2c_buffer,2); //get result
			adc_val_tmp=((i2c_buffer[0]<<8)|(i2c_buffer[1]&0xff))>>4; //combine to int
			adc_value[loop]=adc_val_tmp; //backup new value
			if(adc_center[loop]==-1){adc_center[loop]=adc_val_tmp;} //set center value if not set
			if(adc_min[loop]==-1){adc_min[loop]=adc_val_tmp;} //set min value if not set
			if(adc_val_tmp<adc_min[loop]){adc_min[loop]=adc_val_tmp;} //update min value
			if(adc_max[loop]==-1){adc_max[loop]=adc_val_tmp;} //set max value if not set
			if(adc_val_tmp>adc_max[loop]){adc_max[loop]=adc_val_tmp;} //update max value
			//if(debug){printf("%d : %04d (min: %04d,center: %04d,max: %04d), ",loop,adc_val_tmp,adc_min[loop],adc_center[loop],adc_max[loop]);} //debug
		}
	}else{return ;} //failed to access i2c device
}



int adc_highest_delta(){ //function to report high adc index delta for adc-axis correspondance
	int delta_current=0; //current delta for current loop
	int delta_max=0; //maximum delta detected
	int adc=0; //adc index
	
	for(int delta_loop=0;delta_loop<adc_address_count;delta_loop++){ //loop
		if(adc_skip[delta_loop]==0){ //adc not skipped
			delta_current=adc_center[delta_loop]-adc_value[delta_loop]; //compute current delta
			if(abs(delta_current)>abs(delta_max)){ //highest delta
				delta_max=delta_current; //update max delta
				adc=delta_loop; //update max delta adc index
				if(delta_max<0){adc_reverse[delta_loop]=-1;}else{adc_reverse[delta_loop]=1;} //reverse direction
			}
		}
	}
	return adc; //return adc index
}



int Wait_User_Input(int forbidden_pin,int timeout){ //function to report user input
	long long start=time(NULL); //initial timestamp
	while(true){ //loop until input detected
		if(button_pressed!=-1){ //user pressed a allow key
			button_pressed_tmp=button_pressed; //backup
			button_pressed=-1; //reset gpio button
			if(button_pressed_tmp!=forbidden_pin&&gpio_input_enable[button_pressed_tmp]){return button_pressed_tmp;} //return gpio pin if not a forbidden pin
		}
		
		if(timeout>0&&time(NULL)-start>timeout){return -1;} //return -1 if timeout
		usleep(25*1000); //wait 25msec
	}
}


bool DriverLoaded(){ //function to detect if driver is loaded in lsmod
	temp_filehandle = popen("lsmod | grep mk_arcade_joystick_rpi", "r"); //open process
  if(fread(i2c_buffer,1,sizeof(i2c_buffer),temp_filehandle)>0){return true;} //true if detected
	return false; //false if not detected
}


void show_usage(void){ //usage
	printf(
"Version: %s\n"
"Example : ./mk_joystick_config -debug -maxnoise 60\n"
"Options:\n"
"\t-debug, enable some debug stuff [Optional]\n"
"\t-adcselect, enable user to select ADC chip type [Optional]\n"
"\t-maxnoise, maximum noise allowed for ADC chip, relative from raw analog center, lower value than 60 could create false positive [Optional]\n"
,programversion);
}


int main(int argc, char *argv[]){ //main
	for(int i=1;i<argc;++i){ //argument to variable
		if(strcmp(argv[i],"-help")==0){show_usage();return 1;
		}else if(strcmp(argv[i],"-maxnoise")==0){adc_maxnoise=atoi(argv[i+1]);
		}else if(strcmp(argv[i],"-adcselect")==0){adc_user=false;
		}else if(strcmp(argv[i],"-debug")==0){debug=true;}
	}
	
	setbuf(stdout,NULL); //unbuffered stdout to allow rewrite on the same line
	int tmploop,axisloop; //variables to count loop
	int delta_max=0; //max detected delta
	int adc_useinput=-1; //gpio user input
	int tmpinput=-1; //temp user input
	adc_address_count=sizeof(adc_address)/sizeof(int); //adc_address element count
	
	
	/*
	//temp, could be used in the future, maybe not
	bool config_read=false;
	if(access(config_path,R_OK)!=0){
		printf("\033[91m'%s' not found\033[0m\n",config_path);
		sleep(5);
		return 0;
	}else{
		temp_filehandle=fopen(config_path,"r"); //open file
		while(fgets(text_config,4095,temp_filehandle)!=NULL){ //read line by line
    	if(strstr(text_config,"options mk_arcade_joystick_rpi")!=NULL){ //possible valid line
      	if(strchr(text_config,0x23)==NULL){config_read=true;break;} //line not commented, exit read loop
    	}
		}
		fclose(temp_filehandle); //write file
	}
	
	if(config_read){
		printf("%s\n",text_config);
	}else{
		printf("\033[91mNo valid line found in '%s'\033[0m\n",config_path);
		sleep(5);
		return 0;
	}
	
	return 0;
	*/
	
	
	pthread_create(&gpio_thread, NULL, gpio_routine, NULL); //create routine thread
	sleep(1); while(gpio_thread_rc<0){sleep(2);} //wait until gpio fully initialize
	
	//once gpio is setup, it is needed to read all inputs at least one time to ensure all pullup will work
	scanning_start=time(NULL); //scan start timestamp
	while(time(NULL)-scanning_start<2){button_pressed=-1;} //read for 2sec, reset gpio button
	
	
	printf("\033[1m\033[93m###### Before Start ######\033[0m\n");
	printf("\033[93mVery important: Because this program is design to create a new configuration file from scratch, it can really mess controls if something goes wrong.\n");
	printf("If you think you have make a mistake somewhere, you will be able to restart analog and GPIO part as you want.\n");
	printf("Once the new configuration is setup, you may need to reconfigure input from the EmulationStation settings menu, you should also consider to restart your device.\033[0m\n\n");
	
	
	printf("\033[1m###### Killing MK Arcade Joystick Rpi Driver ######\033[0m\n");
	system("modprobe -r mk_arcade_joystick_rpi");
	sleep(2); //allow some time to kill the driver
	if(DriverLoaded()){printf("\033[91mFailed to kill the driver, exiting\033[0m\n\n"); return 0; //if driver still detected, exit
	}else{printf("\033[92mDriver killed with success\033[0m\n\n");} //continue
	
	
	//buttons for item selection
	printf("\033[1m###### Bind button for User interaction ######\033[0m\n");
	printf("Please press \033[92m(A)\033[0m button once");
	button_pressed=-1; //reset user input
	button_A_gpio=Wait_User_Input(-2,-1); //recover user input
	printf("\033[2K\r\033[92m(A)\033[0m button bind on \033[1mpin %d\033[0m\n",button_A_gpio);
	
	printf("Please press \033[92m(B)\033[0m button once");
	button_B_gpio=Wait_User_Input(button_A_gpio,-1); //recover user input
	printf("\033[2K\r\033[92m(B)\033[0m button bind on \033[1mpin %d\033[0m\n\n",button_B_gpio);
	
	
	printf("\033[1mYou are about to start new configuration\033[0m\n");
	retry=true; //allow continue loop
	while(retry){
		printf("\033[2K\r\033[92m(A)\033[0m to \033[1mContinue\033[0m, \033[92m(B)\033[0m to \033[1mExit\033[0m");
		button_tmp_gpio = Wait_User_Input(-2,-1); //recover user input
		if(button_tmp_gpio==button_A_gpio||button_tmp_gpio==button_B_gpio){ //valid input, ask for validation
			if(button_tmp_gpio==button_A_gpio){printf("\033[2K\r");
			}else if(button_tmp_gpio==button_B_gpio){
				printf("\033[2K\r\033[1mExit without modifications\033[0m\n");
				if(!DriverLoaded()){
					printf("\033[1mRestarting MK Arcade Joystick Rpi Driver\033[0m\n");
					system("modprobe mk_arcade_joystick_rpi"); //system command
				}
				adc_thread_rc=-1; //kill adc thread
				gpio_thread_rc=-1; //kill gpio thread
				sleep(5); //allow some time to start the driver
				return 0; //exit
			}
			retry=false; //disable loop
		}
	}
	printf("\n\n");
	
	
	//start analog part
	printf("\033[1m###### Analog part ######\033[0m\n");
	pthread_create(&adc_thread, NULL, adc_routine, NULL); //create routine thread
	
	scanning_start=time(NULL);
	while(adc_thread_rc<0){ //detection in progress, wait until gpio fully initialize
		if((time(NULL)-scanning_start)>9){ //timeout
			printf("\033[91mADC chip detection timeout (10sec), skipping analog part\033[0m\n\n");
			adc_ads1015=false; adc_mcp3021=false; adc_detected=0; //disable both analog possible chip
			break; //exit while loop
		}else{sleep(2);}
	}
	
	if(adc_mcp3021||adc_ads1015){ //adc chip detected
		printf("\033[1m%d ADC chip(s)\033[0m detected\n",adc_detected);
		if(!adc_user){ //prompt user to select adc chip
			printf("Please select the right type:\n",adc_detected);
			retry=true; //allow continue loop
			while(retry){
				printf("\033[2K\r\033[92m(A)\033[0m for \033[1mMCP3021A\033[0m");
				if(adc_mcp3021){printf(" (Detected)");}
				printf(", \033[92m(B)\033[0m for \033[1mADS1015\033[0m");
				if(adc_ads1015){printf(" (Detected)");}
				
				//button_tmp_gpio = button_B_gpio;
				button_tmp_gpio = Wait_User_Input(-2,-1); //recover user input
				if(button_tmp_gpio==button_A_gpio||button_tmp_gpio==button_B_gpio){ //valid input, ask for validation
					if(button_tmp_gpio==button_A_gpio){printf("\033[2K\rYou selected \033[1mMCP3021A\033[0m? \033[92m(A)\033[0m for \033[1mYes\033[0m, \033[92m(B)\033[0m for \033[1mNo\033[0m");
					}else if(button_tmp_gpio==button_B_gpio){printf("\033[2K\rYou selected \033[1mADS1015\033[0m? \033[92m(A)\033[0m for \033[1mYes\033[0m, \033[92m(B)\033[0m for \033[1mNo\033[0m");}
					
					//button_tmpbis_gpio = button_A_gpio;
					button_tmpbis_gpio = Wait_User_Input(-2,-1); //recover user input
					if(button_tmpbis_gpio==button_A_gpio){ //valid input
						if(button_tmp_gpio==button_A_gpio){printf("\033[2K\r\033[1mMCP3021A\033[0m will be used\n\n");adc_mcp3021=true;adc_ads1015=false;
						}else if(button_tmp_gpio==button_B_gpio){printf("\033[2K\r\033[1mADS1015\033[0m will be used\n\n");adc_mcp3021=false;adc_ads1015=true;}
						adc_user=true; //to notify adc thread of user chip selection
						retry=false; //disable loop
					}
				}
			}
		}else{ //autoselect adc chip
			if(adc_mcp3021){printf("\033[1mMCP3021A\033[0m");}
			else if(adc_ads1015){printf("\033[1mADS1015\033[0m");}
			printf(" chip(s) will be used\n\n");
		}
		
		
		
		
		printf("\033[93mVery important: Calibration in progress\033[0m\n");
		printf("\033[93mPlease touch nothing for \033[1m5sec\033[0m\n");
		scanning_start=time(NULL); //start calibration time
		adc_calibration=true; //start calibration
		while(adc_calibration){sleep(1);} printf("\033[2K\rDone\n"); //calibation
		
		adc_detected=0; //reset adc chip detected count
		for(tmploop=0;tmploop<adc_address_count;tmploop++){ //detect high noise
			if(adc_max[tmploop]-adc_min[tmploop]>adc_maxnoise){ //high noise in input
				printf("\033[91mHigh noise level : %d, ",adc_max[tmploop]-adc_min[tmploop]); //report noise
				if(adc_mcp3021){printf("%d (0x%2x)\n",adc_address[tmploop],adc_address[tmploop]); //mcp
				}else if(adc_ads1015){printf("AIN%d",tmploop);} //ads
				printf(" skipped\033[0m\n");
			}else{
				if(adc_center[tmploop]!=-1){adc_skip[tmploop]=0;adc_detected++;} //if center detected, enable this i2c adress or ain
			}
		}
		printf("\n");
		adc_detected_back=adc_detected; //backup for next part
		
		if(adc_detected<2){ //something wrong with analog
			adc_mcp3021=false;adc_ads1015=false;retrymain=false;
			printf("\033[91mNo enought analog input, Skiping analog part\033[0m\n\n");
		}else{retrymain=true;} //allow analog
		
		for(tmploop=0;tmploop<adc_address_count;tmploop++){adc_skipbackup[tmploop]=adc_skip[tmploop];} //backup adc skip
		
		while(retrymain){
			adc_calibration_done=true;
			button_pressed=-1; //reset gpio button
			adc_detected=adc_detected_back; //restore
			for(tmploop=0;tmploop<adc_address_count;tmploop++){adc_skip[tmploop]=adc_skipbackup[tmploop];} //reset adc skip
			for(axisloop=0;axisloop<4;axisloop++){adc_mapping[axisloop]=-1;} //reset
			
			for(axisloop=0;axisloop<4;axisloop++){ //start axis user definition
				if(adc_detected){ //remaining analog to use
					adc_useinput=-1; delta_max=0; button_pressed=-1; //reset
					
					retry=true;
					while(retry){
						printf("\033[2K\rPush \033[1m%s\033[0m analog \033[1m%s\033[0m then press \033[92m(A)\033[0m, \033[92m(B)\033[0m to \033[1mSkip\033[0m",str_analog_position[axisloop],str_analog_direction[axisloop]);
						button_tmp_gpio = Wait_User_Input(-2,-1); //recover user input
						if(button_tmp_gpio==button_A_gpio||button_tmp_gpio==button_B_gpio){ //valid input, ask for validation
							if(button_tmp_gpio==button_A_gpio){
								printf("\033[2K\r\033[1m%s\033[0m analog \033[1m%s\033[0m axis detected",str_analog_position[axisloop],str_analog_axis[axisloop]);
								
								adc_useinput=adc_highest_delta();
								if(adc_reverse[adc_useinput]<0){printf(", direction reversed");}
								adc_mapping[axisloop]=adc_useinput; //store axis for config creation
								adc_skip[adc_useinput]=1; //disable the detected adc to be used again
								adc_detected--; //reset for the next axis
							}else if(button_tmp_gpio==button_B_gpio){printf("\033[2K\r\033[1m%s\033[0m analog \033[1m%s\033[0m axis skipped",str_analog_position[axisloop],str_analog_axis[axisloop]); adc_useinput==-1;}
							retry=false; //disable loop
						}
					}
					printf("\n\n");
				}
			}
			
			if(debug){printf("x1: %d,y1: %d,x2: %d,y2: %d\n",adc_mapping[0],adc_mapping[1],adc_mapping[2],adc_mapping[3]);} //debug
			for(tmploop=0;tmploop<adc_address_count;tmploop++){adc_min[tmploop]=-1;adc_max[tmploop]=-1;} //reset min/max
			
			if(adc_mapping[0]>-1&&adc_mapping[1]>-1){ //x1 and y1 defined
				printf("\033[2K\r\033[1mSlightly move analogs to their edges\033[0m then press \033[92m(A)\033[0m");
				
				retry=true;
				while(retry){
					button_tmp_gpio = Wait_User_Input(-2,-1); //recover user input
					if(button_tmp_gpio==button_A_gpio){ //valid input
						//sleep(5); //wait 5 sec
						retry=false; //disable loop
					}
				}
				
				printf("\033[2K\r\033[1m%s\033[0m Analog limits :\n",str_analog_position[0]);
				
				printf("\033[1m%s\033[0m : ",str_analog_axis[0]);
				if(adc_mcp3021){printf("\033[1m%d (0x%2x)\033[0m",adc_address[adc_mapping[0]],adc_address[adc_mapping[0]]&0xFF);}else{printf("\033[1mAIN%d\033[0m",adc_mapping[0]);}
				printf(" : min=\033[1m%d\033[0m  max=\033[1m%d\033[0m\n",adc_min[adc_mapping[0]],adc_max[adc_mapping[0]]);
				
				printf("\033[1m%s\033[0m : ",str_analog_axis[1]);
				if(adc_mcp3021){printf("\033[1m%d (0x%2x)\033[0m",adc_address[adc_mapping[1]],adc_address[adc_mapping[1]]&0xFF);}else{printf("\033[1mAIN%d\033[0m",adc_mapping[1]);}
				printf(" : min=\033[1m%d\033[0m  max=\033[1m%d\033[0m\n",adc_min[adc_mapping[1]],adc_max[adc_mapping[1]]);
				
				if(adc_mapping[2]>-1&&adc_mapping[3]>-1){ //x2 and y2 defined
					printf("\033[2K\r\033[1m%s\033[0m Analog limits :\n",str_analog_position[2]);
					
					printf("\033[1m%s\033[0m : ",str_analog_axis[2]);
					if(adc_mcp3021){printf("\033[1m%d (0x%2x)\033[0m",adc_address[adc_mapping[2]],adc_address[adc_mapping[2]]&0xFF);}else{printf("\033[1mAIN%d\033[0m",adc_mapping[2]);}
					printf(" : min=\033[1m%d\033[0m  max=\033[1m%d\033[0m\n",adc_min[adc_mapping[2]],adc_max[adc_mapping[2]]);
				
					printf("\033[1m%s\033[0m : ",str_analog_axis[3]);
					if(adc_mcp3021){printf("\033[1m%d (0x%2x)\033[0m",adc_address[adc_mapping[3]],adc_address[adc_mapping[3]]&0xFF);}else{printf("\033[1mAIN%d\033[0m",adc_mapping[3]);}
					printf(" : min=\033[1m%d\033[0m  max=\033[1m%d\033[0m\n",adc_min[adc_mapping[3]],adc_max[adc_mapping[3]]);
				}else{adc_mapping[2]=-1; adc_mapping[3]=-1;} //to avoid joystick with only one axis
			}else{adc_mapping[0]=-1; adc_mapping[1]=-1;} //to avoid joystick with only one axis
			
			retry=true;
			if(adc_mapping[0]>-1&&adc_mapping[1]>-1){printf("Does theses values look good? \033[92m(A)\033[0m for \033[1mYes\033[0m, \033[92m(B)\033[0m to restart");
			}else{printf("\033[91mAnalog stick require at least 2 inputs\033[0m\n\033[92m(A)\033[0m to \033[1mRestart\033[0m, \033[92m(B)\033[0m to \033[1mSkip Analog part\033[0m");}
			while(retry){
				button_tmp_gpio = Wait_User_Input(-2,-1); //recover user input
				if(button_tmp_gpio==button_A_gpio||button_tmp_gpio==button_B_gpio){ //valid input, ask for validation
					if(button_tmp_gpio==button_A_gpio){
						if(adc_mapping[0]>-1&&adc_mapping[1]>-1){retrymain=false;}
					}else if(button_tmp_gpio==button_B_gpio){
						if(adc_mapping[0]<0||adc_mapping[1]<0){retrymain=false;}
					}
					printf("\n\n");
					retry=false; //disable loop
				}
			}
		}
		
		
		if(debug){printf("x1: %d,y1: %d,x2: %d,y2: %d\n",adc_mapping[0],adc_mapping[1],adc_mapping[2],adc_mapping[3]);} //debug
		
		if(adc_mapping[0]>-1&&adc_mapping[1]>-1){ //x1 and y1 defined, allow selection of analog auto center
			printf("\033[1mAnalog Auto Center\033[0m is a feature that allow non-centered analog(s) stick (eg. PS Vita 1000) to work fine this the driver.\n");
			printf("\033[1mImportant: if enable, you will need to NOT move the analog(s) stick until system fully started.\033[0m\n");
			
			retry=true;
			while(retry){
				printf("\033[2K\rEnable this feature : \033[92m(A)\033[0m for \033[1mYes\033[0m, \033[92m(B)\033[0m for \033[1mNo\033[0m");
				button_tmp_gpio = Wait_User_Input(-2,-1); //recover user input
				if(button_tmp_gpio==button_A_gpio||button_tmp_gpio==button_B_gpio){ //valid input, ask for validation
					if(button_tmp_gpio==button_A_gpio){printf("\033[2K\r\033[1mFeature enabled\033[0m");adc_autocenter=true;
					}else if(button_tmp_gpio==button_B_gpio){printf("\033[2K\r\033[1mFeature disabled\033[0m");adc_autocenter=false;}
					retry=false; //disable loop
				}
			}
			printf("\n\n");
		}
	}else{printf("\033[91mNo analog input detected, Skiping analog part\033[0m\n\n");} //no adc chip
	
	adc_thread_rc=-1; //end adc thread
	
	//gpio part
	printf("\033[1m###### GPIO part ######\033[0m\n");
	
	retrymain=true;
	while(retrymain){
		for(tmploop=0;tmploop<sizeof(button_table)/sizeof(button_table[0]);tmploop++){ //input loop
			if(strcmp(str_button_name[tmploop],"RESERVED")!=0){
				if(tmploop==0){printf("\n\033[93mNote: If (Hotkey) is bind to (Select) in EmulationStation, use the Power Slider as [Hotkey]\033[0m\n");} //hotkey specific
				
				if(tmploop<13){ //needed
					printf("Press \033[92m[%s]\033[0m button",str_button_name[tmploop]);
					tmpinput=Wait_User_Input(-2,-1); //recover user input
				}else{ //optionnal
					printf("Press \033[92m[%s]\033[0m button, wait \033[1m5sec\033[0m to skip",str_button_name[tmploop]);
					tmpinput=Wait_User_Input(-2,5); //recover user input with 5sec timeout
				}
				
				if(tmpinput<0){ //no input
					printf("\033[2K\r\033[92m[%s]\033[0m button skipped\n",str_button_name[tmploop]);
				}else{
					button_table[gpio_button_link_table[tmploop]]=tmpinput; //set input
					printf("\033[2K\r\033[92m[%s]\033[0m button bind on \033[1mpin %d\033[0m\n",str_button_name[tmploop],tmpinput);
					if(gpio_input_activelow[tmpinput]>0){ //set input logic
						button_table_logic[gpio_button_link_table[tmploop]]=-1;
						printf(", reversed logic");
					}
					gpio_input_enable[tmpinput]=false; //disallow user to bind one pin to multiple buttons
					if(gpio_button_link_table[tmploop]==12){ //hotkey button
						button_pressed=-1; //reset gpio button
						retry=true;
						while(retry){
							printf("\033[2K\rPlease select \033[1mHotkey\033[0m mode : \033[92m(A)\033[0m for \033[1mNormal\033[0m, \033[92m(B)\033[0m for \033[1mToggle\033[0m");
							button_tmp_gpio = Wait_User_Input(-2,-1); //recover user input
							if(button_tmp_gpio==button_A_gpio||button_tmp_gpio==button_B_gpio){ //valid input, ask for validation
								if(button_tmp_gpio==button_A_gpio){printf("\033[2K\rHotkey mode set to \033[1mNormal\033[0m\n");button_hkmode=1;
								}else if(button_tmp_gpio==button_B_gpio){printf("\033[2K\rHotkey mode set to \033[1mToggle\033[0m\n");button_hkmode=2;}
								retry=false; //disable loop
							}
						}
					}
				}
				printf("\n");
			}
		}
		
		retry=true;
		for(tmploop=0;tmploop<54;tmploop++){gpio_input_enable[tmploop]=true;} //reset forbidden gpio
		while(retry){
			printf("Press \033[92m(B)\033[0m to \033[1mRestart the GPIO binding\033[0m, \033[92m(A)\033[0m to \033[1mContinue\033[0m");
			button_tmp_gpio = Wait_User_Input(-2,-1); //recover user input
			if(button_tmp_gpio==button_A_gpio||button_tmp_gpio==button_B_gpio){ //valid input, ask for validation
				if(button_tmp_gpio==button_A_gpio){
					retrymain=false;
				}else if(button_tmp_gpio==button_B_gpio){}
				printf("\n\n");
				retry=false; //disable loop
			}
		}
	}
	
	
	printf("\033[1m###### Building configuration file ######\033[0m\n\n");
	
	strcpy(text_config,"options mk_arcade_joystick_rpi map=4"); //start building config file
	sprintf(text_config_buffer, " hkmode=%d",button_hkmode); strcat(text_config,text_config_buffer); //hotkey
	
	if(adc_mapping[0]>-1&&adc_mapping[1]>-1){
		sprintf(text_config_buffer, " i2cbus=%d",1); strcat(text_config,text_config_buffer); //i2cbus
		
		if(adc_autocenter){sprintf(text_config_buffer, " auto_center_analog=%d",1); strcat(text_config,text_config_buffer);} //auto_center_analog
		if(adc_ads1015){sprintf(text_config_buffer, " ads1015addr=%d",adc_address_ads1015); strcat(text_config,text_config_buffer);} //ads1015addr
		
		if(adc_mapping[0]>-1&&adc_mapping[1]>-1){ //x1 and y1 defined
			if(adc_mcp3021){sprintf(text_config_buffer, " x1addr=%d y1addr=%d",adc_address[adc_mapping[0]],adc_address[adc_mapping[1]]); strcat(text_config,text_config_buffer); //x1addr, y1addr for mcp3021
			}else{sprintf(text_config_buffer, " x1addr=%d y1addr=%d",adc_mapping[0],adc_mapping[1]); strcat(text_config,text_config_buffer);} //x1addr, y1addr for ads1015
			
			if(adc_reverse[adc_mapping[0]]<1){
				sprintf(text_config_buffer, " x1dir=%d",-1); strcat(text_config,text_config_buffer); //x1dir
				adc_min[adc_mapping[0]]=4096-adc_min[adc_mapping[0]];
				adc_max[adc_mapping[0]]=4096-adc_max[adc_mapping[0]];
			}
			
			if(adc_min[adc_mapping[0]]>adc_max[adc_mapping[0]]){ //min need to be under max value
				adc_tmp=adc_min[adc_mapping[0]]; //backup
				adc_min[adc_mapping[0]]=adc_max[adc_mapping[0]]; adc_max[adc_mapping[0]]=adc_tmp; //swap
			}
			
			sprintf(text_config_buffer, " x1params=%d,%d,16,300",adc_min[adc_mapping[0]],adc_max[adc_mapping[0]]); strcat(text_config,text_config_buffer); //x1params
			
			if(adc_reverse[adc_mapping[1]]<1){
				sprintf(text_config_buffer, " y1dir=%d",-1); strcat(text_config,text_config_buffer); //y1dir
				adc_min[adc_mapping[1]]=4096-adc_min[adc_mapping[1]];
				adc_max[adc_mapping[1]]=4096-adc_max[adc_mapping[1]];
			}
			
			if(adc_min[adc_mapping[1]]>adc_max[adc_mapping[1]]){ //min need to be under max value
				adc_tmp=adc_min[adc_mapping[0]]; //backup
				adc_min[adc_mapping[1]]=adc_max[adc_mapping[1]]; adc_max[adc_mapping[1]]=adc_tmp; //swap
			}
			
			sprintf(text_config_buffer, " y1params=%d,%d,16,300",adc_min[adc_mapping[1]],adc_max[adc_mapping[1]]); strcat(text_config,text_config_buffer); //y1params
		}
		
		if(adc_mapping[2]>-1&&adc_mapping[3]>-1){ //x2 and y2 defined
			if(adc_mcp3021){sprintf(text_config_buffer, " x2addr=%d y2addr=%d",adc_address[adc_mapping[2]],adc_address[adc_mapping[3]]); strcat(text_config,text_config_buffer); //x2addr, y2addr for mcp3021
			}else{sprintf(text_config_buffer, " x2addr=%d y2addr=%d",adc_mapping[2],adc_mapping[3]); strcat(text_config,text_config_buffer);} //x2addr, y2addr for ads1015
			
			if(adc_reverse[adc_mapping[2]]<1){
				sprintf(text_config_buffer, " x2dir=%d",-1); strcat(text_config,text_config_buffer); //x2dir
				adc_min[adc_mapping[2]]=4096-adc_min[adc_mapping[2]];
				adc_max[adc_mapping[2]]=4096-adc_max[adc_mapping[2]];
			}
			
			if(adc_min[adc_mapping[2]]>adc_max[adc_mapping[2]]){ //min need to be under max value
				adc_tmp=adc_min[adc_mapping[2]]; //backup
				adc_min[adc_mapping[2]]=adc_max[adc_mapping[2]]; adc_max[adc_mapping[2]]=adc_tmp; //swap
			}
			
			sprintf(text_config_buffer, " x2params=%d,%d,16,300",adc_min[adc_mapping[2]],adc_max[adc_mapping[2]]); strcat(text_config,text_config_buffer); //x2params
			
			if(adc_reverse[adc_mapping[3]]<1){
				sprintf(text_config_buffer, " y2dir=%d",-1); strcat(text_config,text_config_buffer); //y2dir
				adc_min[adc_mapping[3]]=4096-adc_min[adc_mapping[3]];
				adc_max[adc_mapping[3]]=4096-adc_max[adc_mapping[3]];
			}
			
			if(adc_min[adc_mapping[3]]>adc_max[adc_mapping[3]]){ //min need to be under max value
				adc_tmp=adc_min[adc_mapping[3]]; //backup
				adc_min[adc_mapping[3]]=adc_max[adc_mapping[3]]; adc_max[adc_mapping[3]]=adc_tmp; //swap
			}
			
			sprintf(text_config_buffer, " y2params=%d,%d,16,300",adc_min[adc_mapping[3]],adc_max[adc_mapping[3]]); strcat(text_config,text_config_buffer); //y2params
		}
	}
	
	
	sprintf(text_config_buffer, " gpio="); strcat(text_config,text_config_buffer); //gpio
	
	for(tmploop=0;tmploop<sizeof(gpio_button_link_table)/sizeof(gpio_button_link_table[0]);tmploop++){ //gpio
		if(tmploop!=0){sprintf(text_config_buffer, ","); strcat(text_config,text_config_buffer);} //add comma
		sprintf(text_config_buffer, "%d", button_table[tmploop]*button_table_logic[tmploop]); strcat(text_config,text_config_buffer); //gpio pin
	}
	
	
	
	
	//save part
	printf("Configuration file sample:\n\033[1m%s\033[0m\n\n",text_config); //display config
	
	retry=true;
	while(retry){
		printf("\033[2K\rSave new configuration? \033[92m(A)\033[0m for \033[1mYes\033[0m, \033[92m(B)\033[0m for \033[1mNo\033[0m");
		
		//button_tmp_gpio = button_B_gpio;
		button_tmp_gpio = Wait_User_Input(-2,-1); //recover user input
		if(button_tmp_gpio==button_A_gpio||button_tmp_gpio==button_B_gpio){ //valid input, ask for validation
			if(button_tmp_gpio==button_A_gpio){printf("\033[2K\rAre you really sure? \033[92m(A)\033[0m for \033[1mYes\033[0m, \033[92m(B)\033[0m for \033[1mNo\033[0m");
			}else if(button_tmp_gpio==button_B_gpio){printf("\033[2K\rExit without saving? \033[92m(A)\033[0m for \033[1mYes\033[0m, \033[92m(B)\033[0m for \033[1mNo\033[0m");}
			
			//button_tmpbis_gpio = button_A_gpio;
			button_tmpbis_gpio = Wait_User_Input(-2,-1); //recover user input
			if(button_tmpbis_gpio==button_A_gpio){ //valid input
				if(button_tmp_gpio==button_A_gpio){save_config=true;
				}else if(button_tmp_gpio==button_B_gpio){save_config=false;}
				retry=false; //disable loop
			}
		}
	}
	printf("\n");
	
	if(save_config){
		if(access(config_path,R_OK)!=0){printf("\033[91m'%s' not found\033[0m\n",config_path);
		}else{
			struct stat file_attribute;
			stat(config_path, &file_attribute);
			sprintf(config_backup_path,"/etc/modprobe.d/mk_arcade_joystick-%d.bak",localtime(&file_attribute.st_mtime)); //create backup full path
			printf("Backup: '\033[1m%s\033[0m'\n",config_backup_path);
			rename(config_path,config_backup_path); //backup config file
		}
		
		temp_filehandle=fopen(config_path,"wb"); //open file
		fprintf(temp_filehandle,"%s",text_config); //write config
		fclose(temp_filehandle); //write file
		printf("Saved: '\033[1m%s\033[0m'\n\n",config_path);
		
		printf("\033[1mRestarting MK Arcade Joystick Rpi Driver\033[0m\n");
		system("modprobe mk_arcade_joystick_rpi"); //system command
		sleep(5); //allow some time to start the driver
		if(DriverLoaded()){
			printf("\033[92mSuccess\033[0m\n\n");
		}else{ //oups
			printf("\033[91mSomething went wrong... Revert back\033[0m\n");
			remove(config_path); //delete now file
			rename(config_backup_path,config_path); //restore config file
			printf("Restored: '%s'\n\n",config_path);
		}
	}else{printf("\033[1mExit without modifications\033[0m\n");}
	
	if(!DriverLoaded()){
		printf("\033[1mRestarting MK Arcade Joystick Rpi Driver\033[0m\n");
		system("modprobe mk_arcade_joystick_rpi"); //system command
	}
	
	adc_thread_rc=-1; //kill adc thread
	gpio_thread_rc=-1; //kill gpio thread
	sleep(5); //allow some time to start the driver
	
	return(0);
}