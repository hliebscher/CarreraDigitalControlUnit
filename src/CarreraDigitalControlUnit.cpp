/*
  Copyright 2017 Thomas Kemmer

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/
#include "CarreraDigitalControlUnit.h"

#include "hal/us_ticker_api.h"
#include "platform/mbed_critical.h"

static const uint16_t emit_mask[10] = {
    1 << 12,
    1 << 9,
    1 << 8,  // or 7 if no command was sent to CU
    1 << 9,
    1 << 9,
    1 << 9,
    1 << 9,
    1 << 9,
    1 << 7,
    1 << 9
};

static unsigned rev12(unsigned data)
{
    data = ((data & 0x555) << 1) | ((data >> 1) & 0x555);
    data = ((data & 0x333) << 2) | ((data >> 2) & 0x333);
    data = ((data & 0xf00) >> 8) | (data & 0x0f0) | ((data & 0x00f) << 8);
    return data;
}

static uint8_t rev8(uint8_t data)
{
    data = ((data & 0x55) << 1) | ((data >> 1) & 0x55);
    data = ((data & 0x33) << 2) | ((data >> 2) & 0x33);
    data = ((data & 0xf0) >> 4) | ((data & 0x0f) << 4);
    return data;
}

CarreraDigitalControlUnit::CarreraDigitalControlUnit(PinName pin, bool inverted)
    : _irq(pin), _data(0), _inverted(inverted), _running(false)
{
}

CarreraDigitalControlUnit::CarreraDigitalControlUnit(PinName pin, PinMode mode, bool inverted)
    : _irq(pin), _data(0), _inverted(inverted), _running(false)
{
    _irq.mode(mode);
}

void CarreraDigitalControlUnit::attach(const Callback<void(int)>& func)
{
    _recv = func;
}

void CarreraDigitalControlUnit::start()
{
    core_util_critical_section_enter();
    if (!_running) {
        reset();
        if (_inverted) {
            _irq.rise(callback(this, &CarreraDigitalControlUnit::fall));
            _irq.fall(callback(this, &CarreraDigitalControlUnit::rise));
        } else {
            _irq.rise(callback(this, &CarreraDigitalControlUnit::rise));
            _irq.fall(callback(this, &CarreraDigitalControlUnit::fall));
        }
        _running = true;
    }
    core_util_critical_section_exit();
}

void CarreraDigitalControlUnit::stop()
{
    core_util_critical_section_enter();
    if (_running) {
        _irq.rise(0);
        _irq.fall(0);
        _running = false;
    }
    core_util_critical_section_exit();
}

void CarreraDigitalControlUnit::reset()
{
    core_util_critical_section_enter();
    _time = 0;
    _buffer = 0;
    _index = 0;
    core_util_critical_section_exit();
}

int CarreraDigitalControlUnit::read()
{
    uint16_t data;
    do {
        data = _data;
    } while (!data || !core_util_atomic_cas_u16(&_data, &data, 0));
    return data;
}

int CarreraDigitalControlUnit::read(long timeout_us)
{
    // read(0) should return data if available
    uint32_t start = us_ticker_read();
    uint16_t data = _data;
    while (!data || !core_util_atomic_cas_u16(&_data, &data, 0)) {
        if ((us_ticker_read() - start) > (uint32_t)timeout_us) {
            return -1;
        }
        data = _data;
    }
    return data;
}

void CarreraDigitalControlUnit::emit()
{
    if (_recv) {
        _recv.call(_buffer);
    }
    _data = _buffer;
    _buffer = 0;
    if (++_index == 10) {
        _index = 0;
    }
}

void CarreraDigitalControlUnit::fall()
{
    // we expect 7.5ms between data packets, so unsigned int should be
    // wide enough on any platform...
    unsigned t = us_ticker_read();
    unsigned d = t - _time;
    if (_buffer && d >= 80 && d < 128) {
        _buffer <<= 1;
        _buffer |= 1;
        if (_buffer & emit_mask[_index]) {
            emit();
        }
        _time = t;
    } else {
        if (_index == 2 && _buffer && d >= 128) {
            emit();  // probably ACT and not ACK
        }
        if (d > 6000) {
            if (_buffer) {
                _index = 0;  // lost sync
            }
            _buffer = 1;
            _time = t;
        }
    }
}

void CarreraDigitalControlUnit::rise()
{
    unsigned t = us_ticker_read();
    unsigned d = t - _time;
    if (d >= 80 && d < 128) {
        _buffer <<= 1;
        if (_buffer & emit_mask[_index]) {
            emit();
        }
        _time = t;
    }
}

bool CarreraDigitalControlUnit::parse_prog(int data, uint8_t res[3])
{
    if ((data & ~0xfff) == 0x1000) {
        // programming word is LSB
        data = rev12(data);
        res[0] = (data >> 4) & 0x1f;
        res[1] = data & 0x0f;
        res[2] = (data >> 9) & 0x07;
        return true;
    } else {
        return false;
    }
}

bool CarreraDigitalControlUnit::parse_ctrl(int data, uint8_t res[4])
{
    if ((data & ~0x1ff) == 0x200 && (data >> 6) != 0xf) {
        // controller word is MSB
        res[0] = (data >> 6) & 0x7;
        res[1] = (data >> 1) & 0xf;
        res[2] = !(data & 0x20);
        res[3] = data & 0x1;
        return true;
    } else {
        return false;
    }
}

bool CarreraDigitalControlUnit::parse_pace(int data, uint8_t res[4])
{
    if ((data & ~0x3f) == 0x3c0) {
        res[0] = (data >> 5) & 0x1;
        res[1] = (data >> 2) & 0x1;
        res[2] = (data >> 1) & 0x1;
        res[3] = data & 0x1;
        return true;
    } else {
        return false;
    }
}

bool CarreraDigitalControlUnit::parse_act(int data, uint8_t res[2])
{
    if ((data & ~0x7f) == 0x80) {
        // active controller mask is LSB
        res[0] = rev8(data >> 1) >> 2;
        res[1] = data & 0x1;
        return true;
    } else {
        return false;
    }
}

bool CarreraDigitalControlUnit::parse_ack(int data, uint8_t res[1])
{
    if ((data & ~0xff) == 0x100) {
        // acknowledge mask is LSB
        res[0] = rev8(data);
        return true;
    } else {
        return false;
    }
}
