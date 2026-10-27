/* audio_codec_eq: ES8311 EQ and state-machine registers; coefficients are signed Q2.28, most significant byte first. */
#pragma once
#define AUDIO_CODEC_EQ_CSM_REG         0x00
#define AUDIO_CODEC_EQ_CSM_ON          0x80u
#define AUDIO_CODEC_EQ_CSM_RESETS      0x1Fu
#define AUDIO_CODEC_EQ_ADC_RAM_REG     0x16
#define AUDIO_CODEC_EQ_ADC_RAMCLR      0x08u
#define AUDIO_CODEC_EQ_DAC_RAM_REG     0x31
#define AUDIO_CODEC_EQ_DAC_RAMCLR      0x08u
#define AUDIO_CODEC_EQ_ADC_HPF_REG     0x1B
#define AUDIO_CODEC_EQ_ADC_CONTROL_REG 0x1C
#define AUDIO_CODEC_EQ_ADC_BYPASS      0x40u
#define AUDIO_CODEC_EQ_ADC_B0_REG      0x1D
#define AUDIO_CODEC_EQ_ADC_A1_REG      0x21
#define AUDIO_CODEC_EQ_ADC_A2_REG      0x25
#define AUDIO_CODEC_EQ_ADC_B1_REG      0x29
#define AUDIO_CODEC_EQ_ADC_B2_REG      0x2D
#define AUDIO_CODEC_EQ_DAC_CONTROL_REG 0x37
#define AUDIO_CODEC_EQ_DAC_BYPASS      0x08u
#define AUDIO_CODEC_EQ_DAC_B0_REG      0x38
#define AUDIO_CODEC_EQ_DAC_B1_REG      0x3C
#define AUDIO_CODEC_EQ_DAC_A1_REG      0x40
#define AUDIO_CODEC_EQ_SCALE           268435456u
#define AUDIO_CODEC_EQ_MASK            0x3FFFFFFFu
#define AUDIO_CODEC_EQ_MIN             (-2.0)
#define AUDIO_CODEC_EQ_BYTES           4u
