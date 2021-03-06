#ifndef COMMUNICATION_H
#define COMMUNICATION_H


/*!
 *   Configuration parameters
 */
struct TelemetryConfig
{
	unsigned char stream_PPM;
	unsigned char stream_GyroAccRaw;
	unsigned char stream_GyroAccProc;
	unsigned char stream_PressureTemp;
	unsigned char stream_GpsBasic;
	unsigned char stream_Attitude;       //! Since 0.4
	unsigned char stream_Control;        //! Since 0.5
};

/*!
 *    FreeRTOS task that sends telemetry to uart1
 */
void communication_telemetry_task( void *parameters );

void communication_input_task( void *parameters );



#endif // COMMUNICATION_H
