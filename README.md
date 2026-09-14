# RTL-SDR Experiments

This repository contains experiments exploring software-defined radio (SDR) using an **RTL-SDR V4** dongle. The projects cover radio reception, television signals, and related digital signal-processing techniques.

## Goals

- Learn the fundamentals of software-defined radio
- Receive and analyze radio signals
- Explore FM, AM, and other broadcast formats
- Experiment with digital television signals
- Build small tools and applications for RTL-SDR

## Hardware

- RTL-SDR V4 USB dongle
- Antennas suitable for the frequencies being received
- Linux computer
- Optional: antenna adapters, filters, and an LNA

## Software

Depending on the experiment, this repository may use:

- `rtl-sdr`
- Python `rtl-sdr` wrapper
- FFmpeg or PaPlay

## Typical Setup

On Debian or Ubuntu-based systems:

```bash
sudo apt update
cmake -B build
cmake --build build --target install_rtllib
cmake --build build --target run_rtltv
cmake --build build --target run_rtlradio
```

Verify that the RTL-SDR device is detected:

```bash
rtl_test
```

Stop any service that may claim the device, such as the DVB-T kernel driver, if necessary.

## Experiments

Planned or completed experiments may include:

- FM broadcast reception
- AM and shortwave signal exploration
- Signal recording and replay
- Frequency scanning
- Digital signal decoding
- Comparison of antennas and filters
- Basic DSP experiments

## General Usage

A basic IQ recording can be created with:

```bash
./build/rtltv -P -c 4 -r 2400000 -g 95 -L 306 -s 0.68 --play-file pongontv.iq
```

The frequency, sample rate, gain, and recording format should be adjusted for each experiment.

## Project Structure

```text
.
├── README.md
├── rtllib/     # Complete and updated librtlsdr library
├── rtlradio/   # Experiments with radio and FM demodulation
└── rtltv/      # Experiments with 80's/90's console RF TV signal
```

## Legal and Safety Notice

Only receive signals that are legal to monitor in your location. Do not transmit, interfere with communications, or decode private communications without authorization. Follow applicable radio, aviation, television, and spectrum regulations.

## Status

This is an experimental and educational project. Documentation and implementations may change as new SDR techniques and hardware are explored.