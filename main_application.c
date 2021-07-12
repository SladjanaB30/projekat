/* Projektni zadatak iz Autoelektronike -"Merenje nivoa goriva u automobilu"
   Studenti: Jelena Sunka EE74/2017
			 Sladjana Babic EE58/2017
   jul 2021.  */

/* Standard includes. */
#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include <string.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"
#include "extint.h"

/* Hardware simulator utility functions */
#include "HW_access.h"

/* SERIAL SIMULATOR CHANNEL TO USE */
#define COM_CH_0 (0)
#define COM_CH_1 (1)

/* DEFINISAN PUN REZERVOAR U LITRIMA */
#define PUN_REZERVOAR 40

/* TASK PRIORITIES */
#define	TASK_SERIAL_SEND_PRI		( tskIDLE_PRIORITY + 2 ) 
#define TASK_SERIAL_REC_PRI			( tskIDLE_PRIORITY + 3 )
#define	SERVICE_TASK_PRI		( tskIDLE_PRIORITY + 1 ) 

/* TASKS: FORWARD DECLARATIONS */
void led_bar_tsk(void* pvParameters);
void merenje_proseka_nivoa_goriva(void* pvParameters);
void nivo_goriva_u_procentima(void* pvParameters);
void SerialSend_Task0(void* pvParameters);
void SerialSend_Task1(void* pvParameters);
void SerialReceive_Task0(void* pvParameters);
void SerialReceive_Task1(void* pvParameters);

/* TRASNMISSION DATA - CONSTANT IN THIS APPLICATION */
static uint8_t MINFUEL;
static uint8_t POTROSNJA;
static uint16_t MAXFUEL;
static double nivo_goriva_procenti=0;
static double autonomija=0;
static int otpornost;

/* RECEPTION DATA BUFFER */
#define R_BUF_SIZE (32)
uint8_t r_buffer[R_BUF_SIZE];
uint8_t r_buffer1[R_BUF_SIZE];
uint8_t brojevi[R_BUF_SIZE];
uint8_t min_otpornost[R_BUF_SIZE];
uint8_t max_otpornost[R_BUF_SIZE];
unsigned volatile r_point,r_point1;

/* 7-SEG NUMBER DATABASE - ALL HEX DIGITS */
static const char hexnum[] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

/* GLOBAL OS-HANDLES */
SemaphoreHandle_t RXC_BinarySemaphore0;
SemaphoreHandle_t RXC_BinarySemaphore1;
QueueHandle_t otpornost_q;

/* Task koji na osnovu pritisnutog tastera ispisuje na 7seg displej informacije, brzina osvezavanja 100ms
   Pritiskom na jedan taster ispisuje nivo goriva u procentima i otpornost
   Pritiskom na drugi taster ispisuje koliko jos kilometara moze da se krece i koliki je rezultat START-STOP naredbe
   START i STOP su realizovani preko tastera  */
void led_bar_tsk(void* pvParameters)
{
	uint8_t d,s;
	int autonomija_int=0;
	int nivo_goriva_procenti_int=0;
	int start_procenti=0;
	int stop_procenti=0;
	int razlika=0;

	for (;;)
	{
		vTaskDelay(pdMS_TO_TICKS(100));

		get_LED_BAR(2, &d);
		get_LED_BAR(3, &s);

		nivo_goriva_procenti_int = (int)nivo_goriva_procenti;

		if (d == 1)
		{
			int jedinica = nivo_goriva_procenti_int % 10;
			int desetica = (nivo_goriva_procenti_int / 10)%10;
			int stotina = nivo_goriva_procenti_int / 100;
			select_7seg_digit(2);
			set_7seg_digit(hexnum[jedinica]);
			select_7seg_digit(1);
			set_7seg_digit(hexnum[desetica]);
			select_7seg_digit(0);
			set_7seg_digit(hexnum[stotina]);

			int jedinica_o = otpornost % 10;
			int desetica_o = (otpornost / 10) % 10;
			int stotina_o = (otpornost / 100) % 10;
			int hiljada_o = (otpornost / 1000) % 10;
			int broj_o = otpornost / 10000;
			select_7seg_digit(8);
			set_7seg_digit(hexnum[jedinica_o]);
			select_7seg_digit(7);
			set_7seg_digit(hexnum[desetica_o]);
			select_7seg_digit(6);
			set_7seg_digit(hexnum[stotina_o]);
			select_7seg_digit(5);
			set_7seg_digit(hexnum[hiljada_o]);
			select_7seg_digit(4);
			set_7seg_digit(hexnum[broj_o]);
		}
		else if (d == 2)
		{
			autonomija_int = (int)autonomija;
			int jedinica = autonomija_int % 10;
			int desetica = (autonomija_int/10)%10;
			int stotina = autonomija_int / 100;
			select_7seg_digit(2);
			set_7seg_digit(hexnum[jedinica]);
			select_7seg_digit(1);
			set_7seg_digit(hexnum[desetica]);
			select_7seg_digit(0);
			set_7seg_digit(hexnum[stotina]);

			int jedinica_r = razlika % 10;
			int desetica_r = razlika / 10;
			select_7seg_digit(8);
			set_7seg_digit(hexnum[jedinica_r]);
			select_7seg_digit(7);
			set_7seg_digit(hexnum[desetica_r]);
		}
		else
		{
			for (int i = 0; i < 9; i++)
			{
				select_7seg_digit(i);
				set_7seg_digit(0x00);
			}
		}
		//aktivan start
		if (s == 128)
		{
			start_procenti = nivo_goriva_procenti_int;
			set_LED_BAR(1, 0x08);
		}
		//aktivan stop
		else if (s == 64)
		{
			set_LED_BAR(1, 0x00);
			stop_procenti = nivo_goriva_procenti_int;
			razlika = start_procenti - stop_procenti;
		}
	}
}

/* Task koji ima za zadatak da meri prosek zadnjih pet pristiglih vrednosti otpornosti */
void merenje_proseka_nivoa_goriva(void* pvParameters) 
{
	int rec_buf,brojac=0,suma=0,prosek;

	for (;;) 
	{
		xQueueReceive(otpornost_q, &rec_buf, portMAX_DELAY); 
		suma = suma + rec_buf;
		brojac++;
		if (brojac == 5)
		{
			prosek = suma / 5;
			//printf("Prosek %d\n", prosek);
			brojac = 0;
			suma = 0;
		}
	}
}

/* Task koji ima za zadatak da preracuna trenutni nivo goriva u procentima i izracuna koliko jos km moze automobil da se krece
sa trenutnom kolicinom goriva */
void nivo_goriva_u_procentima(void* pvParameters)
{
	int rec_buf;
	uint8_t str[3];

	for (;;)
	{
		xQueueReceive(otpornost_q, &rec_buf, portMAX_DELAY); 
		
		/* Tek kada se unesu svi parametri, MINFUEL, MAXFUEL, PP, ispisuje se i racuna se */
		if (MINFUEL != 0 && MAXFUEL != 0)
		{
			nivo_goriva_procenti = (double)100 * (rec_buf - MINFUEL) / (MAXFUEL - MINFUEL);
			printf("U procentima %.1f %% \n", nivo_goriva_procenti);
			if (nivo_goriva_procenti < 10)
			{
				set_LED_BAR(0, 0x01);
			}
			else
				set_LED_BAR(0, 0x00);
		}
		if (POTROSNJA != 0 && MINFUEL != 0 && MAXFUEL != 0)
		{
			autonomija = (double)nivo_goriva_procenti * PUN_REZERVOAR / POTROSNJA;
			printf("Moze jos %.2f km\n", autonomija);
		}
	}
}

/* Task koji vrsi prijem podataka sa kanala 0 - stizu vrednosti otpornosti sa senzora
   Format poruke: R7000.  (u ovom primeru je 7000 vrednost otpornosti)  */
void SerialReceive_Task0(void* pvParameters)
{
	uint8_t cc;

	for (;;) {
		xSemaphoreTake(RXC_BinarySemaphore0, portMAX_DELAY);
		
		get_serial_character(COM_CH_0, &cc);

		//kada stignu podaci, salju se u red
		if (cc == 'R')
		{
			r_point = 0;
		}
		else if (cc == '.')
		{	
			printf(" Otpornost je %s\n", r_buffer);
			otpornost = atoi(r_buffer);
			xQueueSend(otpornost_q, &otpornost, 0);
			
			r_buffer[0] = '\0';
			r_buffer[1] = '\0';
			r_buffer[2] = '\0';
			r_buffer[3] = '\0';
			r_buffer[4] = '\0';
		}
		else if (r_point<R_BUF_SIZE)
		{
			r_buffer[r_point++] = cc; 
		}
	}
}

/* Task koji vrsi prijem podataka sa kanala 1
   Naredbe koje stizu su: MINFUEL<vrednost>, MAXFUEL<vrednost>, PP<vrednost>
   Format poruke: \00<naredba>\0d 
   NPR.
       \00MINFUEL10\0d
	   \00MAXFUEL9000\0d
	   \00PP8\0d                         */
void SerialReceive_Task1(void* pvParameters)
{
	uint8_t cc;
	uint8_t j= 0,k=0,l=0;
	
	for (;;)
	{
		xSemaphoreTake(RXC_BinarySemaphore1, portMAX_DELAY);
		get_serial_character(COM_CH_1, &cc);
		
		if (cc == 0x00)
		{
			j = 0;
			k = 0;
			l = 0;
			r_point1 = 0;
		}
		else if (cc == 13) // 13 decimalno je CR(carriage return)
		{	
			if (r_buffer1[0] == 'M' && r_buffer1[1] == 'I' && r_buffer1[2] == 'N')
			{
				for (int i = 0; i < strlen(r_buffer1); i++)
				{
					//Pristigla naredba je MINFUEL iz koje je potrebno izvuci brojeve
					//Npr. MINFUEL10
					if (r_buffer1[i] == '0' || r_buffer1[i] == '1' || r_buffer1[i] == '2' || r_buffer1[i] == '3' || r_buffer1[i] == '4' || r_buffer1[i] == '5' || r_buffer1[i] == '6' || r_buffer1[i] == '7' || r_buffer1[i] == '8' || r_buffer1[i] == '9')
					{
						min_otpornost[k] = r_buffer1[i];
						k++;
					}
				}
			}
			else if (r_buffer1[0] == 'M' && r_buffer1[1] == 'A' && r_buffer1[2] == 'X')
			{
				for (int i = 0; i < strlen(r_buffer1); i++)
				{
					//Pristigla naredba je MAXFUEL iz koje je potrebno izvuci brojeve
					//Npr. MAXFUEL9000
					if (r_buffer1[i] == '0' || r_buffer1[i] == '1' || r_buffer1[i] == '2' || r_buffer1[i] == '3' || r_buffer1[i] == '4' || r_buffer1[i] == '5' || r_buffer1[i] == '6' || r_buffer1[i] == '7' || r_buffer1[i] == '8' || r_buffer1[i] == '9')
					{
						max_otpornost[l] = r_buffer1[i];
						l++;
					}
				}
			}
			else if (r_buffer1[0] == 'P' && r_buffer1[1] == 'P')
			{
				for (int i = 0; i < strlen(r_buffer1); i++)
				{
					//Pristigla naredba je PP iz koje je potrebno izvuci brojeve
					//Npr. PP8
					if (r_buffer1[i] == '0' || r_buffer1[i] == '1' || r_buffer1[i] == '2' || r_buffer1[i] == '3' || r_buffer1[i] == '4' || r_buffer1[i] == '5' || r_buffer1[i] == '6' || r_buffer1[i] == '7' || r_buffer1[i] == '8' || r_buffer1[i] == '9')
					{
						brojevi[j] = r_buffer1[i];
						j++;
					}
				}
			}

			MINFUEL = atoi(min_otpornost);
			MAXFUEL = atoi(max_otpornost);
			printf("min %d\n", MINFUEL);
			printf("max %d\n", MAXFUEL);
			POTROSNJA = atoi(brojevi);
			printf("potrosnja %d\n", POTROSNJA);

			r_buffer1[0] = '\0';
			r_buffer1[1] = '\0';
			r_buffer1[2] = '\0';
			r_buffer1[3] = '\0';
			r_buffer1[4] = '\0';
			r_buffer1[5] = '\0';
			r_buffer1[6] = '\0';
			r_buffer1[7] = '\0';
			r_buffer1[8] = '\0';
			r_buffer1[9] = '\0';
			r_buffer1[10] = '\0';
			r_buffer1[11] = '\0';
		}
		else
		{
			r_buffer1[r_point1++] = cc;
		}
	}
}

/* Sa ovim taskom simuliramo vrednosti otpornosti koje stizu sa senzora svakih 1s, tako sto
   svakih 1s saljemo karakter 'a' i u AdvUniCom simulatoru omogucimo tu opciju (AUTO ukljucen) */
void SerialSend_Task0(void* pvParameters)
{
	uint8_t c='a';
	
	for (;;)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
		send_serial_character(COM_CH_0,c);
	}
}

/* Ovim taskom preko kanala 1 saljemo vrednost nivoa goriva u procentima, svakih 1s */
void SerialSend_Task1(void* pvParameters)
{
	uint8_t str[6];
	static uint8_t brojac = 0;

	for (;;)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
		
		sprintf(str, "%.1f%%\n",nivo_goriva_procenti);
		brojac++;

		//format stringa npr. 33.3%\n
		if (MINFUEL != 0 && MAXFUEL != 0)
		{
			if (brojac == 1)
				send_serial_character(COM_CH_1, str[0]);
			else if (brojac == 2)
				send_serial_character(COM_CH_1, str[1]);
			else if (brojac == 3)
				send_serial_character(COM_CH_1, str[2]);
			else if (brojac == 4)
				send_serial_character(COM_CH_1, str[3]);
			else if (brojac == 5)
				send_serial_character(COM_CH_1, str[4]);
			else if (brojac == 6)
				send_serial_character(COM_CH_1, str[5]);
			else
				brojac = 0;
		}
	}
}

/* Interrupt rutina za serijsku komunikaciju u kojoj se proverava koji je kanal poslao i na osnovu toga daje odgovarajuci semafor */
uint32_t prvProcessRXCInterrupt(void)
{
	BaseType_t higher_priority_task_woken = pdFALSE;

	if (get_RXC_status(0) != 0)
		xSemaphoreGiveFromISR(RXC_BinarySemaphore0, &higher_priority_task_woken);

	if (get_RXC_status(1) != 0)
		xSemaphoreGiveFromISR(RXC_BinarySemaphore1, &higher_priority_task_woken);

	portYIELD_FROM_ISR(higher_priority_task_woken);
}

/* MAIN - SYSTEM STARTUP POINT */
void main_demo(void)
{
	/* Inicijalizacija periferija */
	init_LED_comm();
	init_7seg_comm();
	init_serial_uplink(COM_CH_0); // inicijalizacija serijske TX na kanalu 0
	init_serial_uplink(COM_CH_1); // inicijalizacija serijske TX na kanalu 1
	init_serial_downlink(COM_CH_0);// inicijalizacija serijske RX na kanalu 0
	init_serial_downlink(COM_CH_1);// inicijalizacija serijske RX na kanalu 1

	/* SERIAL RECEPTION INTERRUPT HANDLER */
	vPortSetInterruptHandler(portINTERRUPT_SRL_RXC, prvProcessRXCInterrupt);//interrupt za serijsku prijem

	/* Kreiranje semafora */
	RXC_BinarySemaphore0 = xSemaphoreCreateBinary();
	if (RXC_BinarySemaphore0 == NULL) 
	{
		while (1);
	}

	RXC_BinarySemaphore1 = xSemaphoreCreateBinary();
	if (RXC_BinarySemaphore1 == NULL)
	{
		while (1);
	}

	/*Kreiranje reda*/
	otpornost_q = xQueueCreate(10, sizeof(int));
	if (otpornost_q == NULL)
	{
		while (1);
	}

	/*Kreiranje taskova*/
	BaseType_t status;
	status = xTaskCreate(
		merenje_proseka_nivoa_goriva,
		"merenje task",
		configMINIMAL_STACK_SIZE,
		NULL,
		SERVICE_TASK_PRI,
		NULL
	);
	if (status == NULL)
	{
		while (1);
	}

	status = xTaskCreate(
		nivo_goriva_u_procentima,
		"procenti task",
		configMINIMAL_STACK_SIZE,
		NULL,
		SERVICE_TASK_PRI,
		NULL
	);
	if (status == NULL)
	{
		while (1);
	}

	/* SERIAL RECEIVER AND SEND TASK */
	status=xTaskCreate(SerialReceive_Task0, "SRx", configMINIMAL_STACK_SIZE, NULL, TASK_SERIAL_REC_PRI, NULL);
	if (status == NULL)
	{
		while (1);
	}

	status=xTaskCreate(SerialReceive_Task1, "SRx", configMINIMAL_STACK_SIZE, NULL, TASK_SERIAL_REC_PRI, NULL);
	if (status == NULL)
	{
		while (1);
	}
	
	status=xTaskCreate(SerialSend_Task0, "STx", configMINIMAL_STACK_SIZE, NULL, TASK_SERIAL_SEND_PRI, NULL);
	if (status == NULL)
	{
		while (1);
	}

	status=xTaskCreate(SerialSend_Task1, "STx", configMINIMAL_STACK_SIZE, NULL, TASK_SERIAL_SEND_PRI, NULL);
	if (status == NULL)
	{
		while (1);
	}
	
	status=xTaskCreate(led_bar_tsk, "ST", configMINIMAL_STACK_SIZE, NULL, SERVICE_TASK_PRI, NULL);
	if (status == NULL)
	{
		while (1);
	}

	r_point = 0;
	r_point1 = 0;

    vTaskStartScheduler();
	
	while (1);
}