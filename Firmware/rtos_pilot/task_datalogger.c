/*
 *
 *    Dataflash:
 *    [ Page 0 | Page 1 | Page 2 | Page 3 | ... | Page 4096 ]
 *    |  Configuration  | Index+1| Logging .................|
 *     
 *     Logging page=    
 *    [ 2Bytes: index number (0 = unused) | Bytes 3...528: Array of LogLines.... ]
 *
 */
 
 
#include <stdio.h>

// Include all FreeRTOS header files
#include "FreeRTOS/FreeRTOS.h"
#include "FreeRTOS/task.h"
#include "FreeRTOS/queue.h"
#include "FreeRTOS/croutine.h"
#include "FreeRTOS/semphr.h"

#include "microcontroller/microcontroller.h"
#include "dataflash/dataflash.h"
#include "uart1_queue/uart1_queue.h"

#include "task_datalogger.h"
#include "sensors.h"
#include "task_control.h"
#include "gluonscript.h"
#include "handler_navigation.h"
#include "handler_trigger.h"
#include "common.h"


struct LogIndex datalogger_index_table[MAX_INDEX];

unsigned char buffer[528];  // WARNING: won't work with AT45DB321


int current_page;
int current_index = 0;

int disable_logging = 0; // used when reading out data


xSemaphoreHandle xSpiSemaphore;
void datalogger_read(int page, int size, unsigned char *buffer);
void datalogger_write(int page, int size, unsigned char *buffer);

/*!
 *   Initializes the datalogging (to dataflash) functionality.
 *
 *   First the current index page is read out.
 *   Then we look for the oldest index. 
 *   Then we look for the youngest page
 *   Then we take the first page after the youngest page, that is not from the youngest index
 */
void datalogger_init()
{
	int i, start_page = START_LOG_PAGE, last_index = -1;
	unsigned long date2 = 0xFFFFFFFF;
	unsigned long date;
	
	current_page = START_LOG_PAGE;
	
	//printf("formatting...");
	//datalogger_format();
	//printf("done");
	
	// read index page
	datalogger_read(LOG_INDEX_PAGE, sizeof(struct LogIndex) * MAX_INDEX, (unsigned char*)datalogger_index_table);
	
	// find the index with the oldest date: date
	for (i = 0; i < MAX_INDEX; i++)
	{
		date = (datalogger_index_table[i].date % 100) * 100000000 + 
		       ((datalogger_index_table[i].date % 10000) / 100) * 1000000 + 
		       ((datalogger_index_table[i].date / 10000) * 10000) +
		       datalogger_index_table[i].time / 100;
		if (date < date2)
		{
			date2 = date;
			//start_page = datalogger_index_table[i].page_num + 1;
			current_index = (i)%MAX_INDEX + 1;
		}
	}
	
	// find the index with the youngest date: we need to find the first available page after the one used by the youngest index
	date2 = 0;
	for (i = 0; i < MAX_INDEX; i++)
	{
		date = (datalogger_index_table[i].date % 100) * 100000000 + 
		       ((datalogger_index_table[i].date % 10000) / 100) * 1000000 + 
		       ((datalogger_index_table[i].date / 10000) * 10000) +
		       datalogger_index_table[i].time / 100;
		if (date > date2)
		{
			date2 = date;
			start_page = datalogger_index_table[i].page_num + 1;
		}
	}	
	
	if (start_page < START_LOG_PAGE)  // possible after a format
		start_page = START_LOG_PAGE;

	// Lets find the first page	that is
	//    - 0: not been written yet: OK
	//    - (current index) = page written by this index: the previous index didn't overwrite it: OK
	//    - A change in index: this is the one we will need to sacrifice...
	for (i = start_page; i < MAX_PAGE; i++)
	{
		int *index;
		datalogger_read(i, 4, buffer);
		index = (int*) &(buffer[0]);
		if (*index == 0 || *index == (current_index) || (*index != last_index && last_index > 0))
		{
			current_page = i;
			*index = (current_index); // buffer[0] + [1] contains current_index + 1
			break;
		}
		last_index = *index;
	}

    
	//printf("index %d , page %d\r\n", current_index, current_page);
}


/*!
 *    This function is called when the GPS (date & time!) is available and the index page can be written.
 */ 
void datalogger_start_session()
{	
	// update the index
	datalogger_index_table[current_index - 1].page_num = current_page;
	if (control_state.simulation_mode)
	{
		// set using Enable Simulation command
		datalogger_index_table[current_index - 1].time = sensor_data.gps.time;
		datalogger_index_table[current_index - 1].date = sensor_data.gps.date;
	}
	else
	{
		datalogger_index_table[current_index - 1].time = sensor_data.gps.time;
		datalogger_index_table[current_index - 1].date = sensor_data.gps.date;
	}
	datalogger_write(LOG_INDEX_PAGE, sizeof(struct LogIndex) * MAX_INDEX, (unsigned char*)datalogger_index_table);
	//printf("Starting to datalog to page %d, index %d\r\n", current_page, current_index);
}	


void datalogger_read(int page, int size, unsigned char *buffer)
{
	if (xSemaphoreTake( xSpiSemaphore, ( portTickType ) 0 ) == pdTRUE )   // Spi1 is shared with SCP1000 and Dataflash
	{
		dataflash.read(page, size, buffer);
		xSemaphoreGive( xSpiSemaphore );
	} else
        printf("\r\nSPI Flash not available\r\n");
}	

void datalogger_write(int page, int size, unsigned char *buffer)
{
	if (xSemaphoreTake( xSpiSemaphore, ( portTickType ) 0 ) == pdTRUE )   // Spi1 is shared with SCP1000 and Dataflash
	{
		dataflash.write(page, size, buffer);
		xSemaphoreGive( xSpiSemaphore );
        //printf("\r\nSPI Flash available\r\n");
    } else
        printf("\r\nSPI Flash not available\r\n");
}	


int current_line = 0;
/*!
 *    Save the LogLine line to a buffer. Write the buffer when full.
 */
void datalogger_writeline(struct LogLine *line)
{
	unsigned char *a, *b;
	int i;
	struct LogLine *lines = (struct LogLine*) &(buffer[2]);   // buffer[0-1] set in datalogger_init()
	
	if (2+(current_line+1)*sizeof(struct LogLine) >= PAGE_SIZE)
	{
		datalogger_write(current_page, sizeof(buffer), buffer);
		current_page++;
		if (current_page >= MAX_PAGE)
			current_page = START_LOG_PAGE;
		current_line = 0;
		//printf("write page!\n\r");
	}	
	
	a = (unsigned char*) line;
	b = (unsigned char*) &(lines[current_line++]);
	
	for (i = 0; i < sizeof(struct LogLine); i++)
		b[i] = a[i];
}


//static long last_totalseconds = -1;
/*!
 *    Prints the contents of the next available page on "index" using the
 *    "printer" function.
 *    
 *    @index   The index (see dataflash page 3) we want to read.
 *    @printer The function used to format the LogLine according to the current
 *             used communication protocol.
 */
int datalogger_print_next_page(int index, void(*printer)(struct LogLine*))
{
	static int last_index = -1;
	static int last_page = -1;
	static struct LogLine *lines = (struct LogLine*) &(buffer[2]);
	
	int *i = (int*) &(buffer[0]);
	int j;
	
	if (index != last_index)
	{
		last_page = datalogger_index_table[index].page_num;
		last_index = index;
	}

	datalogger_read(last_page++, sizeof(buffer), buffer);
	if (last_page >= MAX_PAGE)
			last_page = START_LOG_PAGE;

#ifdef DETAILED_LOG //|| RAW_50HZ_LOG
    if (*i != index+1)
	{
		printf ("%d != %d\r\n", *i, index+1);
		return 0;
	}
#else
    /*long hours = (lines[0].time / 10000);
    long minutes = (lines[0].time % 10000) / 100;
    long seconds = (lines[0].time) % 100;
	long totalseconds =  seconds + minutes * 60 + hours * 3600;
    if (abs(totalseconds  - last_totalseconds) > 2 && last_totalseconds != -1)
    {
        printf("\r\n Stop: new line: %lu  %lu \r\n", lines[0].date, lines[0].time);
        return 0;
    }
    hours = (lines[PAGE_SIZE / sizeof(struct LogLine) - 2].time / 10000);
    minutes = (lines[PAGE_SIZE / sizeof(struct LogLine) - 2].time % 10000) / 100;
    seconds = (lines[PAGE_SIZE / sizeof(struct LogLine) - 2].time) % 100;
	totalseconds =  seconds + minutes * 60 + hours * 3600;
    last_totalseconds = totalseconds;*/
    if (*i != index+1)
	{
		printf ("%d != %d\r\n", *i, index+1);
		return 0;
	}
#endif

	for (j = 0; j < (PAGE_SIZE - 2) / sizeof(struct LogLine); j++)
    {
        if (lines[j].gps_latitude_rad < DEG2RAD(360.0) && lines[j].gps_longitude_rad < DEG2RAD(360.0) )
            printer(&lines[j]);
        else
            return 0;
    }
		
	return 1;
}

int datalogger_print_next_page_of_all(int index, void(*printer)(struct LogLine*))
{
	static int last_page = -1;
	static struct LogLine *lines = (struct LogLine*) &(buffer[2]);
	static int processed_lines = 0;

	int *current_i = (int*) &(buffer[0]);
	int j;

	if (last_page == -1)
		last_page = datalogger_index_table[index].page_num; // start to read at the current page (oldest entry to be overwritten)

	datalogger_read(last_page++, sizeof(buffer), buffer);
	processed_lines++;

	if (last_page == datalogger_index_table[index].page_num || processed_lines > 4095 - START_LOG_PAGE)
	{
		printf("\r\nAll log-pages have been processed\r\n");
		processed_lines = 0;
		last_page = -1;
		return 0;
	}
	if (last_page >= MAX_PAGE)
			last_page = START_LOG_PAGE;

	/*if (*current_i != index+1)
	{
		printf ("%d != %d\r\n", *current_i, index+1);
		return 0;
	}*/

	for (j = 0; j < (PAGE_SIZE - 2) / sizeof(struct LogLine); j++)
	{
		if (*current_i == index)
			printer(&lines[j]);
	}
	printf("\r\n processed page %d having index %d\r\n", last_page, *current_i);
	return 1;
}
	

/*!
 *   Formats all dataflash pages except the first 2 (used for configuration)
 */
void datalogger_format()
{
	int i;
	for (i = 0; i < PAGE_SIZE; i++)
		buffer[i] = 0;
		
	for (i = LOG_INDEX_PAGE; i <= MAX_PAGE; i++)
	{
		printf("page %d\r\n", i);
		datalogger_write(i, PAGE_SIZE, buffer);
	}	
	
	datalogger_init();
}	


void datalogger_disable()
{
	disable_logging = 1;
}


void datalogger_enable()
{
	disable_logging = 0;
}


/*!
 *    This task takes care of the logging, both initialization and actual logging.
 * 
 *    The initialization of the logging index (page 3) starts when a valid GPS frame
 *    is available. This is needed because the date & time are stored in the index.
 *
 *    Measured used stack space: 274 / 1290 bytes
 */
void datalogger_task( void *parameters )
{
	static struct LogLine l;
	
	/* Used to wake the task at the correct frequency. */
	portTickType xLastExecutionTime; 

	uart1_puts("Datalogger task initializing...");

    vTaskSetApplicationTaskTag( NULL, ( void * ) 5 );
	datalogger_init();
	
	uart1_puts("done\r\n");
	
	/* Initialise xLastExecutionTime so the first call to vTaskDelayUntil()	works correctly. */
	xLastExecutionTime = xTaskGetTickCount();
	
	// wait for GPS	(date & time!)
	while(sensor_data.gps.status != ACTIVE)
		vTaskDelayUntil( &xLastExecutionTime, ( ( portTickType ) 1000 / portTICK_RATE_MS ) );   // 1Hz
	
	// ok, now we've got the current date and time, we can find an available page and write the index	
	datalogger_start_session();
	
	for( ;; )
	{	
#ifndef RAW_50HZ_LOG
		vTaskDelayUntil( &xLastExecutionTime, ( ( portTickType ) 250 / portTICK_RATE_MS ) );   // 4Hz
#else
		vTaskDelayUntil( &xLastExecutionTime, ( ( portTickType ) 20 / portTICK_RATE_MS ) );   // 50Hz
#endif		

		if (! disable_logging)   // logging is disabled when the config tool reads out logging.
		{
#ifdef DETAILED_LOG
			// Normal logging
			l.temperature_c = (char)sensor_data.temperature; // -128�C...+128�C
			l.height_m = (int)sensor_data.pressure_height;
			l.gps_latitude_rad = sensor_data.gps.latitude_rad;
			l.gps_longitude_rad = sensor_data.gps.longitude_rad;
			l.gps_height_m = sensor_data.gps.height_m;
			l.gps_heading = (int)(sensor_data.gps.heading_rad * (180.0/3.14159));
			l.gps_speed_m_s = (int)(sensor_data.gps.speed_ms*100.0);
			l.gps_satellites = (char)sensor_data.gps.satellites_in_view;
			//l.acc_x = sensor_data.acc_x_raw;
			//l.acc_y = sensor_data.acc_y_raw;
			//l.acc_z = sensor_data.acc_z_raw;
			l.acc_x_g = sensor_data.acc_x;
			l.acc_y_g = sensor_data.acc_y;
			l.acc_z_g = sensor_data.acc_z;

			//l.gyro_x = sensor_data.gyro_x_raw;
			//l.gyro_y = sensor_data.gyro_y_raw;
			//l.gyro_z = sensor_data.gyro_z_raw;
			l.p = (int)(sensor_data.p * (180.0/3.14159));
			l.q = (int)(sensor_data.q * (180.0/3.14159));
			l.r = (int)(sensor_data.r * (180.0/3.14159));
			l.pitch = (int)(sensor_data.pitch * (180.0/3.14159));
			l.roll = (int)(sensor_data.roll * (180.0/3.14159));
			l.desired_pitch = (int)(control_state.desired_pitch * (180.0/3.14159));
			l.desired_roll = (int)(control_state.desired_roll * (180.0/3.14159));
			//l.pitch_acc = (int)(sensor_data.pitch_acc * (180.0/3.14159));
			//l.roll_acc = (int)(sensor_data.roll_acc * (180.0/3.14159));
			l.control_state = control_state.flight_mode;
			l.desired_heading = ((int)(navigation_data.desired_heading_rad * 180.0/3.14159));
			l.navigation_code_line = gluonscript_data.current_codeline;
			l.desired_height = control_state.desired_altitude;
#elif RAW_50HZ_LOG
			// Raw sensor logging at 50Hz
			l.height_m_5 = (int)(sensor_data.pressure_height*5);
			l.gps_latitude_rad = sensor_data.gps.latitude_rad;
			l.gps_longitude_rad = sensor_data.gps.longitude_rad;
			l.gps_heading_2 = (unsigned char)(sensor_data.gps.heading_rad * (180.0/3.14159) / 2.0);
			l.gps_speed_m_s_10 = (unsigned char)(sensor_data.gps.speed_ms * 10.0);
			l.gps_time = sensor_data.gps.time;
			l.acc_x = sensor_data.acc_x_raw;
			l.acc_y = sensor_data.acc_y_raw;
			l.acc_z = sensor_data.acc_z_raw;
			l.gyro_x = sensor_data.gyro_x_raw;
			l.gyro_y = sensor_data.gyro_y_raw;
			l.gyro_z = sensor_data.gyro_z_raw;
			//l.idg500_vref = sensor_data.idg500_vref;
			l.pitch = (int)(sensor_data.pitch * (180.0/3.14159));
			l.pitch_acc = (int)(sensor_data.pitch_acc * (180.0/3.14159));
			l.roll = (int)(sensor_data.roll * (180.0/3.14159));
			//l.control_state = control_state.flight_mode;

#else
            // Simple logging
			l.temperature_c = (char)sensor_data.temperature; // -128�C...+128�C
			l.height_m = (int)sensor_data.pressure_height;
			l.gps_latitude_rad = sensor_data.gps.latitude_rad;
			l.gps_longitude_rad = sensor_data.gps.longitude_rad;
			l.gps_height_m = sensor_data.gps.height_m;
			l.gps_heading = (int)(sensor_data.gps.heading_rad * (180.0/3.14159));
			l.gps_speed_m_s = (unsigned char)(sensor_data.gps.speed_ms*3.0);

			l.pitch = (int)(sensor_data.pitch * (180.0/3.14159));
			l.roll = (int)(sensor_data.roll * (180.0/3.14159));
            l.yaw = (int)(sensor_data.yaw * (180.0/3.14159));
			l.control_state = control_state.flight_mode;
			l.navigation_code_line = gluonscript_data.current_codeline;
            l.date = sensor_data.gps.date;
            l.time = sensor_data.gps.time;
            l.servo_trigger = trigger.trigger_counter;
#endif
			datalogger_writeline(&l);

		}
        else // logging disabled:
        {
            printf("\r\nLogging task stopped\r\n");
            vTaskDelete(NULL);
        }
	}
}
