#include "stm32f0xx.h"
#include <math.h>
#include <stdio.h>

#ifndef M_PI
    #define M_PI 3.141592
#endif

void nano_wait(unsigned int);
void internal_clock();
void drive_column(int);
int  read_rows();
void update_history(int col, int rows);
char get_key_event(void);
char get_keypress(void);
void show_keys(void);
void spi1_dma_display1(const char *str);
void spi1_dma_display2(const char *str);
void print(const char str[]);

void autotune_algorithm(uint64_t note, char** tuned_name, uint32_t* tuned_frequency);
int abs_fn(int n);


//============================================================================
// Input raw note via the ADC
//============================================================================
// sampling parameters
#define BUFFER_SIZE 1024
#define SAMPLE_RATE 24000
uint16_t bufferIndex = 0;
int adcBuffer[BUFFER_SIZE];
uint32_t frequency;

void setup_adc(void) {
    // enable RCC clock on GPIO A
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN;

    // configure ADC_IN1 to be in analog mode (pin PA1)
    GPIOA->MODER |= 0xc;

    // enable ADC clock
    RCC->APB2ENR |= RCC_APB2ENR_ADCEN;

    // continuous mode
    ADC1->CFGR1 |= ADC_CFGR1_CONT;

    // enable high-speed 14MHz clock (HSI14)
    RCC->CR2 |= RCC_CR2_HSI14ON;

    // wait for HSI14 to be ready
    while(!(RCC->CR2 & RCC_CR2_HSI14RDY));

    // enable ADC by setting ADEN bit
    ADC1->CR |= ADC_CR_ADEN;

    // wait for ADC1 to be ready
    while(!(ADC1->ISR & ADC_ISR_ADRDY));

    // select channel for ADC_IN1
    ADC1->CHSELR |= 1 << 1;

    // wait for ADC1 to be ready
    while(!(ADC1->ISR & ADC_ISR_ADRDY));

    ADC1->CR |= ADC_CR_ADSTART;
}

int note = 0;

void TIM2_IRQHandler(void) {
    TIM2->SR &= ~TIM_SR_UIF;
    while (!(ADC1->ISR & ADC_ISR_EOC));

    adcBuffer[bufferIndex] = ADC1->DR;
    
    note = adcBuffer[bufferIndex];
    
    bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
}

void init_tim2(void) {
    // enable RCC clock for TIM2
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

    // lower TIM2 priority
    NVIC_SetPriority(TIM2_IRQn, 3);

    // prescale to 4.8MHz
    TIM2->PSC = 9;
    // auto-reload register to 48kHz
    TIM2->ARR = 199;

    // enable UIE bit in the DIER
    TIM2->DIER |= TIM_DIER_UIE;

    // enable TIM2 interrupt in NVIC
    NVIC_EnableIRQ(TIM2_IRQn);

    // enable TIM2 by setting CEN bit in TIM2_CR1
    TIM2->CR1 |= TIM_CR1_CEN;
}


uint64_t dominantFrequency; 

void performFFT(void) {
    // arrays to hold real and imaginary parts

    float real[BUFFER_SIZE];
    float imag[BUFFER_SIZE];

    // Copy adcBuffer into real part, set imaginary part to zero
    for (int i = 0; i < BUFFER_SIZE; i++) {
        real[i] = (float)adcBuffer[i];
        imag[i] = 0.0f;
    }

    // Bit-reversal 
    int n = BUFFER_SIZE;
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (i < j) {
            // Swap real[i] and real[j]
            float tempReal = real[i];
            real[i] = real[j];
            real[j] = tempReal;

            // Swap imag[i] and imag[j]
            float tempImag = imag[i];
            imag[i] = imag[j];
            imag[j] = tempImag;
        }
        int m = n / 2;
        while (j >= m && m > 0) {
            j -= m;
            m /= 2;
        }
        j += m;
    }

    // Danielson-Lanczos algorithm
    int nu = 10; // Since BUFFER_SIZE is 256, log2(256) = 8
    for (int s = 1; s <= nu; s++) {
        int m = 1 << s;
        int m2 = m / 2;
        float theta = -M_PI / m2;
        float wpr = cosf(theta);
        float wpi = sinf(theta);

        for (int k = 0; k < n; k += m) {
            float wr = 1.0f;
            float wi = 0.0f;

            for (int j = 0; j < m2; j++) {
                int t = k + j + m2;
                int u = k + j;

                float tr = wr * real[t] - wi * imag[t];
                float ti = wr * imag[t] + wi * real[t];

                real[t] = real[u] - tr;
                imag[t] = imag[u] - ti;

                real[u] += tr;
                imag[u] += ti;

                // Update the twiddle factors
                float tempWr = wr;
                wr = tempWr * wpr - wi * wpi;
                wi = tempWr * wpi + wi * wpr;
            }
        }
    }

    // Find magnitude spectrum and find dominant frequency
    float maxMagnitude = 0.0f;
    int dominantBin = 0;
    for (int i = 1; i < BUFFER_SIZE / 2; i++) {
        // Find magnitude
        float magnitude = real[i] * real[i] + imag[i] * imag[i];

        if (magnitude > maxMagnitude) {
            maxMagnitude = magnitude;
            dominantBin = i;
        }
    }

    // Find dominant frequency and scale by 10000

    dominantFrequency = (dominantBin * SAMPLE_RATE) / BUFFER_SIZE;
}

//============================================================================
// Autotune the note
//============================================================================

#define OCTAVES 9
#define NOTES_PER_OCTAVE 12

const int note_frequencies[OCTAVES][NOTES_PER_OCTAVE] = {
    {163500, 173200, 183500, 194500, 206000, 218300, 231200, 245000, 259600, 275000, 291400, 308700},
    {327000, 346500, 367100, 388900, 412000, 436500, 462500, 490000, 519100, 550000, 582700, 617400},
    {654100, 693000, 734200, 777800, 824100, 873100, 925000, 980000, 1038300, 1100000, 1165400, 1234700},
    {1308100, 1385900, 1468300, 1555600, 1648100, 1746100, 1850000, 1960000, 2076500, 2200000, 2330800, 2469400},
    {2616300, 2771800, 2936600, 3111300, 3296300, 3492300, 3699900, 3920000, 4153000, 4400000, 4661600, 4938800},
    {5232500, 5543700, 5873300, 6222500, 6592500, 6984600, 7399900, 7839900, 8306100, 8800000, 9323300, 9877700},
    {10465000, 11087300, 11746600, 12445100, 13185100, 13969100, 14799800, 15679800, 16612200, 17600000, 18646600, 19755300},
    {20930000, 22174600, 23493200, 24890200, 26370200, 27938300, 29599600, 31359600, 33224400, 35200000, 37293100, 39510700},
    {41860100, 44349200, 46986300, 49780300, 52740400, 55876500, 59199100, 62719300, 66448800, 70400000, 74586200, 79021300} 
};

void autotune_algorithm(uint64_t note, char** tuned_name, uint32_t* tuned_frequency) {
    char* name_arr[NOTES_PER_OCTAVE] = {"C ", "Db", "D ", "Eb", "E ", "F ", "Gb", "G ", "Ab", "A ", "Bb", "B "};

    int octave_reduce = 0; // number of times frequency is halved
    uint64_t converted_note = note;
    while (converted_note > 317900) { // halfway between octave 0 B and octave 1 C
        converted_note /= 2;
        octave_reduce++;
    }

    // Find the nearest note
    double min_difference = abs_fn(converted_note - note_frequencies[0][0]);
    int note_idx = 0;

    for (int i = 1; i < NOTES_PER_OCTAVE; i++) {
        double temp_difference = abs_fn(converted_note - note_frequencies[0][i]);
        if (temp_difference < min_difference) {
            min_difference = temp_difference;
            note_idx = i;
        }
    }
    
    *tuned_name = name_arr[note_idx];
    *tuned_frequency = note_frequencies[octave_reduce][note_idx];
}

int abs_fn(int n) {
    return (n < 0) ? -n : n;
}

//============================================================================

// Parameters for the wavetable size and expected synthesis rate.
#define N 1000
#define RATE 20000
int volume = 2048;
short int wavetable[N];
int step0 = 0;
int offset0 = 0;
int step1 = 0;
int offset1 = 0;

//===========================================================================
// init_wavetable()
// Write the pattern for a complete cycle of a sine wave into the
// wavetable[] array.
//===========================================================================
void init_wavetable(void) {
    for(int i=0; i < N; i++)
        wavetable[i] = 32767 * sin(2 * M_PI * i / N);
}

//============================================================================
// set_freq()
//============================================================================
void set_freq(int chan, float f) {
    if (chan == 0) {
        if (f == 0.0) {
            step0 = 0;
            offset0 = 0;
        } else
            step0 = (f * N / RATE) * (1<<16);
    }
    if (chan == 1) {
        if (f == 0.0) {
            step1 = 0;
            offset1 = 0;
        } else
            step1 = (f * N / RATE) * (1<<16);
    }
}

//============================================================================
// setup_dac()
//============================================================================
void setup_dac(void) {
    //DAC_OUT1 is PA4
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN; // enable the clock to GPIO A
    GPIOA->MODER |= 0b1100000000; // set DAC_OUT1 to analog mode
    RCC->APB1ENR |= RCC_APB1ENR_DACEN; // enable the clock for DAC peripheral
    DAC->CR |= 0b000101; // Select TIM6 TRGO trigger and enable the trigger for the DAC, then enable DAC
}

//============================================================================
// Timer 6 ISR
//============================================================================
// Write the Timer 6 ISR here.  Be sure to give it the right name.
void TIM6_DAC_IRQHandler(void) {
    TIM6->SR &= ~TIM_SR_UIF; //acknowledge the interrupt
    set_freq(0,(float)frequency / 10000);
    offset0 += step0;
    offset1 += step1;
    if (offset0 >= (N << 16)) {
        offset0 -= (N << 16);
    }
    if (offset1 >= (N << 16)) {
        offset1 -= (N << 16);
    }
    int samp = wavetable[offset0 >> 16] + wavetable[offset1 >> 16];
    samp *= volume;
    samp = samp >> 17;
    samp += 2048;
    DAC->DHR12R1 = samp;
}

//============================================================================
// init_tim6()
//============================================================================
void init_tim6(void) {
    RCC->APB1ENR |= RCC_APB1ENR_TIM6EN;
    TIM6->PSC = (48000000 / 1000000) - 1; // gets timer to 1 MHz
    TIM6->ARR = (1000000 / RATE) - 1; // gets timer to RATE Hz
    TIM6->DIER |= TIM_DIER_UIE; // enable update interrupt every time counter reaches ARR value
    NVIC_EnableIRQ(TIM6_DAC_IRQn); // enable interrupt
   
    TIM6->CR2 |= 0b0100000; // MMS field for Update event
    TIM6->CR1 |= TIM_CR1_CEN;
}


//============================================================================
// Display note using the 7-segment display
// nOtE__XY, where X is the note letter and Y is sharp or flat
//============================================================================
uint16_t msg[8] = { 0x0000,0x0100,0x0200,0x0300,0x0400,0x0500,0x0600,0x0700 };
uint8_t col;

void init_tim7(void) {
    // enable RCC clock for TIM7
    RCC->APB1ENR |= RCC_APB1ENR_TIM7EN;

    // prescale to 10kHz
    TIM7->PSC = 4799;
    // auto-reload register to 1kHz
    TIM7->ARR = 9;

    // enable UIE bit in the DIER
    TIM7->DIER |= TIM_DIER_UIE;

    // enable TIM7 interrupt in NVIC
    NVIC_EnableIRQ(TIM7_IRQn);

    // enable TIM7 by setting CEN bit in TIM7_CR1
    TIM7->CR1 |= TIM_CR1_CEN;
}

void TIM7_IRQHandler(void) {
    // acknowledge timer interrupt by clearing UIF bit
    TIM7->SR &= ~TIM_SR_UIF;

    // update button history
    int rows = read_rows();
    update_history(col, rows);
    col = (col + 1) & 3;
    drive_column(col);
}

void init_spi2(void) {
    // enable RCC clock for GPIOB and SPI2
    RCC->AHBENR |= RCC_AHBENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;

    // pins PB12, PB13, PB15 set to alt function mode
    GPIOB->MODER &= ~(0xFFFFFFFF);
    GPIOB->MODER |= GPIO_MODER_MODER12_1 | GPIO_MODER_MODER13_1 | GPIO_MODER_MODER15_1;

    // ensure SPI2 is disabled
    SPI2->CR1 &= ~SPI_CR1_SPE;

    // set baud rate to lowest possible setting
    SPI2->CR1 |= SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0;

    // set data frame format to 16-bit
    SPI2->CR2 |= (15 << SPI_CR2_DS_Pos);

    // configure SPI2 as master device
    SPI2->CR1 |= SPI_CR1_MSTR;

    // set SS output enable and enable NSSP
    SPI2->CR2 |= SPI_CR2_SSOE | SPI_CR2_NSSP;

    // enable DMA transmit on buffer empty
    SPI2->CR2 |= SPI_CR2_TXDMAEN;

    // enable SPI2 peripheral
    SPI2->CR1 |= SPI_CR1_SPE;
}

void spi2_setup_dma(void) {
    // enable RCC clock for DMA1
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;

    // ensure DMA channel is disabled
    DMA1_Channel5->CCR &= ~DMA_CCR_EN;

    // setup channel memory address register
    DMA1_Channel5->CMAR = (uint32_t) (msg);

    // setup channel peripheral address register
    DMA1_Channel5->CPAR = (uint32_t) &(SPI2->DR);

    // setup channel number of data to transfer register
    DMA1_Channel5->CNDTR = 8;

    // setup copy direction
    DMA1_Channel5->CCR |= DMA_CCR_DIR; // read from memory

    // setup memory increment
    DMA1_Channel5->CCR |= DMA_CCR_MINC;

    // setup memory datum size as 16 bits
    DMA1_Channel5->CCR |= DMA_CCR_MSIZE_0; // 00: 8-bit, --01: 16-bit--, 10: 32-bit

    // setup peripheral datum size as 16 bits
    DMA1_Channel5->CCR |= DMA_CCR_PSIZE_0; // 00: 8-bit, --01: 16-bit--, 10: 32-bit

    // setup channel for circular operation
    DMA1_Channel5->CCR |= DMA_CCR_CIRC; 

    // enable SPI2 bit
    SPI2->CR2 |= SPI_CR2_TXEIE;
}

void spi2_enable_dma(void) {
    DMA1_Channel5->CCR |= DMA_CCR_EN;
}

//============================================================================
// Display input and output note frequency using the OLED display
// Input:  456.28hz
// Output: 440.00hz
//============================================================================
uint16_t display[34] = {
        0x002, // Command to set the cursor at the first position line 1
        0x200+'E', 0x200+'C', 0x200+'E', 0x200+'3', 0x200+'6', + 0x200+'2', 0x200+' ', 0x200+'i',
        0x200+'s', 0x200+' ', 0x200+'t', 0x200+'h', + 0x200+'e', 0x200+' ', 0x200+' ', 0x200+' ',
        0x0c0, // Command to set the cursor at the first position line 2
        0x200+'c', 0x200+'l', 0x200+'a', 0x200+'s', 0x200+'s', + 0x200+' ', 0x200+'f', 0x200+'o',
        0x200+'r', 0x200+' ', 0x200+'y', 0x200+'o', + 0x200+'u', 0x200+'!', 0x200+' ', 0x200+' ',
};

void init_spi1(void) {
    // enable RCC clock for GPIOA and SPI1
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;

    // pins PA15, PA5, PA7 set to alt function mode
    GPIOA->MODER &= ~(0xFFFFFFFF);
    GPIOA->MODER |= 0x28000000; // restore important pin values
    GPIOA->MODER |= GPIO_MODER_MODER15_1 | GPIO_MODER_MODER5_1 | GPIO_MODER_MODER7_1;

    // ensure SPI1 is disabled
    SPI1->CR1 &= ~SPI_CR1_SPE;

    // set baud rate to lowest possible setting
    SPI1->CR1 |= SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0;

    // set data frame format to 10-bit
    SPI1->CR2 |= (15 << SPI_CR2_DS_Pos);
    SPI1->CR2 &= ~(6 << SPI_CR2_DS_Pos);

    // configure SPI1 as master device
    SPI1->CR1 |= SPI_CR1_MSTR;

    // set SS output enable and enable NSSP
    SPI1->CR2 |= SPI_CR2_SSOE | SPI_CR2_NSSP;

    // enable DMA transmit on buffer empty
    SPI1->CR2 |= SPI_CR2_TXDMAEN;

    // enable SPI1 peripheral
    SPI1->CR1 |= SPI_CR1_SPE;
}

void spi_cmd(unsigned int data) {
    // wait until SPI1 TX buffer is empty
    while (!(SPI1->SR & SPI_SR_TXE)) {;}

    // send data
    SPI1->DR = data;
}

void spi_data(unsigned int data) {
    // calls spi_cmd with data
    spi_cmd(data | 0x200);
}

void spi1_init_oled() {
    // wait 1 ms
    nano_wait(1000000);

    // send initialization commands
    spi_cmd(0x38);       // function set
    spi_cmd(0x08);       // display off
    spi_cmd(0x01);       // clear display
    nano_wait(2000000);  // wait for 2 ms
    spi_cmd(0x06);       // set entry mode
    spi_cmd(0x02);       // move cursor to home position
    spi_cmd(0x0C);       // display on
}

void spi1_setup_dma(void) {
    // enable RCC clock for DMA1
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;

    // ensure DMA channel is disabled
    DMA1_Channel3->CCR &= ~DMA_CCR_EN;

    // setup channel memory address register
    DMA1_Channel3->CMAR = (uint32_t) (display);

    // setup channel peripheral address register
    DMA1_Channel3->CPAR = (uint32_t) &(SPI1->DR);

    // setup channel number of data to transfer register
    DMA1_Channel3->CNDTR = 34;

    // setup copy direction
    DMA1_Channel3->CCR |= DMA_CCR_DIR; // read from memory

    // setup memory increment
    DMA1_Channel3->CCR |= DMA_CCR_MINC;

    // setup memory datum size as 16 bits
    DMA1_Channel3->CCR |= DMA_CCR_MSIZE_0; // 00: 8-bit, --01: 16-bit--, 10: 32-bit

    // setup peripheral datum size as 16 bits
    DMA1_Channel3->CCR |= DMA_CCR_PSIZE_0; // 00: 8-bit, --01: 16-bit--, 10: 32-bit

    // setup channel for circular operation
    DMA1_Channel3->CCR |= DMA_CCR_CIRC; 

    // enable SPI1 bit
    SPI1->CR2 |= SPI_CR2_TXDMAEN;
}

void spi1_enable_dma(void) {
    DMA1_Channel3->CCR |= DMA_CCR_EN;
}

//============================================================================
// Run the program!
//============================================================================
char* name = "A ";

char note_display[8] = "Note A ";
char frequency_str[16];
char note_str[16];

int main(void)
{
    // setup
    internal_clock();

    // enable 7-SEG display
    init_spi2();
    spi2_setup_dma();
    spi2_enable_dma();
    // enable OLED display
    init_spi1();
    spi1_init_oled();
    spi1_setup_dma();
    spi1_enable_dma();
    // Enable ADC
    setup_adc();
    init_tim2();
    

    // Enable DAC
    init_wavetable();
    setup_dac();
    init_tim6();

    while(1) {
        if (bufferIndex == BUFFER_SIZE - 1) {
            performFFT();
            // Run autotune algorithm to get the note and frequency
            autotune_algorithm(dominantFrequency * 10000, &name, &frequency);

            // Display on 7-SEG
            note_display[5] = name[0];
            note_display[6] = name[1];
            print(note_display);

            // Display on OLED
            sprintf(frequency_str, "Output: %ldhz", frequency / 10000);
            sprintf(note_str, "Input: %dhz", dominantFrequency);

            spi1_dma_display1(note_str);
            spi1_dma_display2(frequency_str);
        }
    }

}
