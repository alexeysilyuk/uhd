//
// Copyright 2017 Ettus Research (National Instruments)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "ad937x_device.hpp"
#include "adi/mykonos.h"
#include "adi/mykonos_gpio.h"
#include "adi/mykonos_debug/mykonos_dbgjesd.h"

#include <boost/format.hpp>

#include <functional>
#include <iostream>
#include <thread>
#include <fstream>

using namespace mpm::ad937x::device;
using namespace mpm::ad937x::gpio;
using namespace uhd;

const double ad937x_device::MIN_FREQ = 300e6;
const double ad937x_device::MAX_FREQ = 6e9;
const double ad937x_device::MIN_RX_GAIN = 0.0;
const double ad937x_device::MAX_RX_GAIN = 30.0;
const double ad937x_device::RX_GAIN_STEP = 0.5;
const double ad937x_device::MIN_TX_GAIN = 0.0;
const double ad937x_device::MAX_TX_GAIN = 41.95;
const double ad937x_device::TX_GAIN_STEP = 0.05;

static const double RX_DEFAULT_FREQ = 2.5e9;
static const double TX_DEFAULT_FREQ = 2.5e9;
static const double RX_DEFAULT_GAIN = 0;
static const double TX_DEFAULT_GAIN = 0;

static const uint32_t AD9371_PRODUCT_ID = 0x3;
static const size_t ARM_BINARY_SIZE = 98304;

static const size_t PLL_LOCK_TIMEOUT_MS = 200;
static const uint32_t INIT_CAL_TIMEOUT_MS = 10000;

// TODO: actually figure out what cals we want to run
// minimum required cals are 0x4F
static const uint32_t INIT_CALS =
    TX_BB_FILTER |
    ADC_TUNER |
    TIA_3DB_CORNER |
    DC_OFFSET |
//    TX_ATTENUATION_DELAY |
//    RX_GAIN_DELAY |
    FLASH_CAL |
//    PATH_DELAY |
//    TX_LO_LEAKAGE_INTERNAL |
////  TX_LO_LEAKAGE_EXTERNAL |
//    TX_QEC_INIT |
//    LOOPBACK_RX_LO_DELAY |
//    LOOPBACK_RX_RX_QEC_INIT |
//    RX_LO_DELAY |
//    RX_QEC_INIT |
////  DPD_INIT |
////  CLGC_INIT |
////  VSWR_INIT |
    0;

static const uint32_t TRACKING_CALS =
//    TRACK_RX1_QEC |
//    TRACK_RX2_QEC |
//    TRACK_ORX1_QEC |
//    TRACK_ORX2_QEC |
////  TRACK_TX1_LOL |
////  TRACK_TX2_LOL |
//    TRACK_TX1_QEC |
//    TRACK_TX2_QEC |
////  TRACK_TX1_DPD |
////  TRACK_TX2_DPD |
////  TRACK_TX1_CLGC |
////  TRACK_TX2_CLGC |
////  TRACK_TX1_VSWR |
////  TRACK_TX2_VSWR |
////  TRACK_ORX1_QEC_SNLO |
////  TRACK_ORX2_QEC_SNLO |
////  TRACK_SRX_QEC |
    0;



/******************************************************
Helper functions
******************************************************/

// helper function to unify error handling
void ad937x_device::_call_api_function(std::function<mykonosErr_t()> func)
{
    auto error = func();
    if (error != MYKONOS_ERR_OK)
    {
        throw mpm::runtime_error(getMykonosErrorMessage(error));
    }
}

// helper function to unify error handling, GPIO version
void ad937x_device::_call_gpio_api_function(std::function<mykonosGpioErr_t()> func)
{
    auto error = func();
    if (error != MYKONOS_ERR_GPIO_OK)
    {
        throw mpm::runtime_error(getGpioMykonosErrorMessage(error));
    }
}

// _move_to_config_state() and _restore_from_config_state() are a pair of functions
// that should be called at the beginning and end (respectively) of any configuration
// function that requires the AD9371 to be in the radioOff state.  _restore should be
// called with the return value of _move.

// read the current state of the AD9371 and change it to radioOff (READY)
// returns the current state, to later be consumed by _restore_from_config_state()
ad937x_device::radio_state_t ad937x_device::_move_to_config_state()
{
    uint32_t status;
    _call_api_function(std::bind(MYKONOS_getRadioState, mykonos_config.device, &status));
    if ((status & 0x3) == 0x3)
    {
        stop_radio();
        return radio_state_t::ON;
    }
    else {
        return radio_state_t::OFF;
    }
}

// restores the state from before a call to _move_to_config_state
// if ON, move to radioOn, otherwise this function is a no-op
void ad937x_device::_restore_from_config_state(ad937x_device::radio_state_t state)
{
    if (state == radio_state_t::ON)
    {
        start_radio();
    }
}

std::string ad937x_device::_get_arm_binary_path()
{
    // TODO: possibly add more options here, for now it's always in /lib/firmware or we explode
    return "/lib/firmware/Mykonos_M3.bin";
}

std::vector<uint8_t> ad937x_device::_get_arm_binary()
{
    auto path = _get_arm_binary_path();
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        throw mpm::runtime_error("Could not open AD9371 ARM binary at path " + path);
        return {};
    }

    // TODO: add check that opened file size is equal to ARM_BINARY_SIZE
    std::vector<uint8_t> binary(ARM_BINARY_SIZE);
    file.read(reinterpret_cast<char*>(binary.data()), ARM_BINARY_SIZE);
    if (file.bad())
    {
        throw mpm::runtime_error("Error reading AD9371 ARM binary at path " + path);
        return {};
    }
    return binary;
}

void ad937x_device::_verify_product_id()
{
    uint8_t product_id = get_product_id();
    if (product_id != AD9371_PRODUCT_ID)
    {
        throw mpm::runtime_error(str(
            boost::format("AD9371 product ID does not match expected ID! Read: %X Expected: %X")
            % int(product_id) % int(AD9371_PRODUCT_ID)
        ));
    }
}

void ad937x_device::_verify_multichip_sync_status(multichip_sync_t mcs)
{
    uint8_t status_expected = (mcs == multichip_sync_t::FULL) ? 0x0B : 0x0A;
    uint8_t status_mask = status_expected; // all 1s expected, mask is the same

    uint8_t mcs_status = get_multichip_sync_status();
    if ((mcs_status & status_mask) != status_expected)
    {
        throw mpm::runtime_error(str(boost::format("Multichip sync failed! Read: %X Expected: %X")
            % int(mcs_status) % int(status_expected)));
    }
}

// RX Gain values are table entries given in mykonos_user.h
// An array of gain values is programmed at initialization, which the API will then use for its gain values
// In general, Gain Value = (255 - Gain Table Index)
uint8_t ad937x_device::_convert_rx_gain_to_mykonos(double gain)
{
    // TODO: derive 195 constant from gain table
    // gain should be a value 0-60, add 195 to make 195-255
    return static_cast<uint8_t>((gain * 2) + 195);
}

double ad937x_device::_convert_rx_gain_from_mykonos(uint8_t gain)
{
    return (static_cast<double>(gain) - 195) / 2.0;
}

// TX gain is completely different from RX gain for no good reason so deal with it
// TX is set as attenuation using a value from 0-41950 mdB
// Only increments of 50 mdB are valid
uint16_t ad937x_device::_convert_tx_gain_to_mykonos(double gain)
{
    // attenuation is inverted and in mdB not dB
    return static_cast<uint16_t>((MAX_TX_GAIN - (gain)) * 1e3);
}

double ad937x_device::_convert_tx_gain_from_mykonos(uint16_t gain)
{
    return (MAX_TX_GAIN - (static_cast<double>(gain) / 1e3));
}

void ad937x_device::_apply_gain_pins(direction_t direction, chain_t chain)
{
    using namespace std::placeholders;

    // get this channels configuration
    auto chan = gain_ctrl.config.at(direction).at(chain);

    // TX direction does not support different steps per direction
    if (direction == TX_DIRECTION)
    {
        MPM_ASSERT_THROW(chan.inc_step == chan.dec_step);
    }

    auto state = _move_to_config_state();

    switch (direction)
    {
        case RX_DIRECTION:
        {
            std::function<decltype(MYKONOS_setRx1GainCtrlPin)> func;
            switch (chain)
            {
            case chain_t::ONE:
                func = MYKONOS_setRx1GainCtrlPin;
                break;
            case chain_t::TWO:
                func = MYKONOS_setRx2GainCtrlPin;
                break;
            }
            _call_gpio_api_function(
                std::bind(func,
                    mykonos_config.device,
                    chan.inc_step,
                    chan.dec_step,
                    chan.inc_pin,
                    chan.dec_pin,
                    chan.enable));
            break;
        }
        case TX_DIRECTION:
        {
            // TX sets attenuation, but the configuration should be stored correctly
            std::function<decltype(MYKONOS_setTx2AttenCtrlPin)> func;
            switch (chain)
            {
            case chain_t::ONE:
                // TX1 has an extra parameter "useTx1ForTx2" that we do not support
                func = std::bind(MYKONOS_setTx1AttenCtrlPin, _1, _2, _3, _4, _5, 0);
                break;
            case chain_t::TWO:
                func = MYKONOS_setTx2AttenCtrlPin;
                break;
            }
            _call_gpio_api_function(
                std::bind(func,
                    mykonos_config.device,
                    chan.inc_step,
                    chan.inc_pin,
                    chan.dec_pin,
                    chan.enable));
            break;
        }
    }
    _restore_from_config_state(state);
}



/******************************************************
Initialization functions
******************************************************/

ad937x_device::ad937x_device(
    mpm::types::regs_iface* iface,
    gain_pins_t gain_pins) :
    full_spi_settings(iface),
    mykonos_config(&full_spi_settings.spi_settings),
    gain_ctrl(gain_pins)
{
}

void ad937x_device::_initialize_rf()
{
    // TODO: add setRfPllLoopFilter here when we upgrade the API to >3546

    // Set frequencies
    tune(uhd::RX_DIRECTION, RX_DEFAULT_FREQ, false);
    tune(uhd::TX_DIRECTION, TX_DEFAULT_FREQ, false);

    if (!get_pll_lock_status(CLK_SYNTH | RX_SYNTH | TX_SYNTH | SNIFF_SYNTH, true))
    {
        throw mpm::runtime_error("PLLs did not lock after initial tuning!");
    }

    // Set gain control GPIO pins
    _apply_gain_pins(uhd::RX_DIRECTION, chain_t::ONE);
    _apply_gain_pins(uhd::RX_DIRECTION, chain_t::TWO);
    _apply_gain_pins(uhd::TX_DIRECTION, chain_t::ONE);
    _apply_gain_pins(uhd::TX_DIRECTION, chain_t::TWO);

    _call_gpio_api_function(std::bind(MYKONOS_setupGpio, mykonos_config.device));

    // Set manual gain values
    set_gain(uhd::RX_DIRECTION, chain_t::ONE, RX_DEFAULT_GAIN);
    set_gain(uhd::RX_DIRECTION, chain_t::TWO, RX_DEFAULT_GAIN);
    set_gain(uhd::TX_DIRECTION, chain_t::ONE, TX_DEFAULT_GAIN);
    set_gain(uhd::TX_DIRECTION, chain_t::TWO, TX_DEFAULT_GAIN);

    // Run and wait for init cals
    _call_api_function(std::bind(MYKONOS_runInitCals, mykonos_config.device, INIT_CALS));

    uint8_t errorFlag = 0, errorCode = 0;
    _call_api_function(std::bind(MYKONOS_waitInitCals, mykonos_config.device, INIT_CAL_TIMEOUT_MS, &errorFlag, &errorCode));

    if ((errorFlag != 0) || (errorCode != 0))
    {
        throw mpm::runtime_error("Init cals failed!");
        // TODO: add more debugging information here
    }

    _call_api_function(std::bind(MYKONOS_enableTrackingCals, mykonos_config.device, TRACKING_CALS));
    // ready for radioOn
}

void ad937x_device::begin_initialization()
{
    _call_api_function(std::bind(MYKONOS_initialize, mykonos_config.device));

    _verify_product_id();

    if (!get_pll_lock_status(CLK_SYNTH))
    {
        throw mpm::runtime_error("AD937x CLK_SYNTH PLL failed to lock");
    }

    uint8_t mcs_status = 0;
    _call_api_function(std::bind(MYKONOS_enableMultichipSync, mykonos_config.device, 1, &mcs_status));
}

void ad937x_device::finish_initialization()
{
    _verify_multichip_sync_status(multichip_sync_t::PARTIAL);

    _call_api_function(std::bind(MYKONOS_initArm, mykonos_config.device));
    auto binary = _get_arm_binary();
    _call_api_function(std::bind(MYKONOS_loadArmFromBinary,
        mykonos_config.device,
        binary.data(),
        binary.size()));

    _initialize_rf();
}

void ad937x_device::start_jesd_tx()
{
    _call_api_function(std::bind(MYKONOS_enableSysrefToRxFramer, mykonos_config.device, 1));
}

void ad937x_device::start_jesd_rx()
{
    _call_api_function(std::bind(MYKONOS_enableSysrefToDeframer, mykonos_config.device, 0));
    _call_api_function(std::bind(MYKONOS_resetDeframer, mykonos_config.device));
    _call_api_function(std::bind(MYKONOS_enableSysrefToDeframer, mykonos_config.device, 1));
}

void ad937x_device::start_radio()
{
    _call_api_function(std::bind(MYKONOS_radioOn, mykonos_config.device));
}

void ad937x_device::stop_radio()
{
    _call_api_function(std::bind(MYKONOS_radioOff, mykonos_config.device));
}



/******************************************************
Get status functions
******************************************************/

uint8_t ad937x_device::get_multichip_sync_status()
{
    uint8_t mcs_status = 0;
    // to check status, just call the enable function with a 0 instead of a 1, seems good
    _call_api_function(std::bind(MYKONOS_enableMultichipSync, mykonos_config.device, 0, &mcs_status));
    return mcs_status;
}

uint8_t ad937x_device::get_framer_status()
{
    uint8_t status = 0;
    _call_api_function(std::bind(MYKONOS_readRxFramerStatus, mykonos_config.device, &status));
    return status;
}

uint8_t ad937x_device::get_deframer_status()
{
    uint8_t status = 0;
    _call_api_function(std::bind(MYKONOS_readDeframerStatus, mykonos_config.device, &status));
    return status;
}

uint16_t ad937x_device::get_ilas_config_match()
{
    uint16_t ilas_status = 0;
    _call_api_function(std::bind(MYKONOS_jesd204bIlasCheck, mykonos_config.device, &ilas_status));
    return ilas_status;
}

uint8_t ad937x_device::get_product_id()
{
    uint8_t id;
    _call_api_function(std::bind(MYKONOS_getProductId, mykonos_config.device, &id));
    return id;
}

uint8_t ad937x_device::get_device_rev()
{
    uint8_t rev;
    _call_api_function(std::bind(MYKONOS_getDeviceRev, mykonos_config.device, &rev));
    return rev;
}

api_version_t ad937x_device::get_api_version()
{
    api_version_t api;
    _call_api_function(std::bind(MYKONOS_getApiVersion,
        mykonos_config.device,
        &api.silicon_ver,
        &api.major_ver,
        &api.minor_ver,
        &api.build_ver));
    return api;
}

arm_version_t ad937x_device::get_arm_version()
{
    arm_version_t arm;
    _call_api_function(std::bind(MYKONOS_getArmVersion,
        mykonos_config.device,
        &arm.major_ver,
        &arm.minor_ver,
        &arm.rc_ver));
    return arm;
}



/******************************************************
Set configuration functions
******************************************************/

void ad937x_device::enable_jesd_loopback(uint8_t enable)
{
    auto state = _move_to_config_state();
    _call_api_function(std::bind(MYKONOS_setRxFramerDataSource, mykonos_config.device, enable));
    _restore_from_config_state(state);
}

double ad937x_device::set_clock_rate(double req_rate)
{
    auto rate = static_cast<uint32_t>(req_rate / 1000.0);

    auto state = _move_to_config_state();
    mykonos_config.device->clocks->deviceClock_kHz = rate;
    _call_api_function(std::bind(MYKONOS_initDigitalClocks, mykonos_config.device));
    _restore_from_config_state(state);

    return static_cast<double>(rate);
}

void ad937x_device::enable_channel(direction_t direction, chain_t chain, bool enable)
{
    // TODO:
    // Turns out the only code in the API that actually sets the channel enable settings
    // is _initialize(). Need to figure out how to deal with this.
    // mmeserve 8/24/2017
    // While it is possible to change the enable state after disabling the radio, we'll probably
    // always use the GPIO pins to do so. Delete this function at a later time.
}

double ad937x_device::tune(direction_t direction, double value, bool wait_for_lock = false)
{
    // I'm not sure why we set the PLL value in the config AND as a function parameter
    // but here it is

    mykonosRfPllName_t pll;
    uint8_t locked_pll;
    uint64_t* config_value;
    uint64_t integer_value = static_cast<uint64_t>(value);
    switch (direction)
    {
    case TX_DIRECTION:
        pll = TX_PLL;
        locked_pll = TX_SYNTH;
        config_value = &(mykonos_config.device->tx->txPllLoFrequency_Hz);
        break;
    case RX_DIRECTION:
        pll = RX_PLL;
        locked_pll = RX_SYNTH;
        config_value = &(mykonos_config.device->rx->rxPllLoFrequency_Hz);
        break;
    default:
        MPM_THROW_INVALID_CODE_PATH();
    }

    auto state = _move_to_config_state();
    *config_value = integer_value;
    _call_api_function(std::bind(MYKONOS_setRfPllFrequency, mykonos_config.device, pll, integer_value));

    if (wait_for_lock)
    {
        if (!get_pll_lock_status(locked_pll, true))
        {
            throw mpm::runtime_error("PLL did not lock");
        }
    }
    _restore_from_config_state(state);

    return get_freq(direction);
}

double ad937x_device::set_bw_filter(direction_t direction, chain_t chain, double value)
{
    // TODO: implement
    return double();
}


double ad937x_device::set_gain(direction_t direction, chain_t chain, double value)
{
    double coerced_value;
    auto state = _move_to_config_state();
    switch (direction)
    {
    case TX_DIRECTION:
    {
        uint16_t attenuation = _convert_tx_gain_to_mykonos(value);
        coerced_value = static_cast<double>(attenuation);

        std::function<mykonosErr_t(mykonosDevice_t*, uint16_t)> func;
        switch (chain)
        {
        case chain_t::ONE:
            func = MYKONOS_setTx1Attenuation;
            break;
        case chain_t::TWO:
            func = MYKONOS_setTx2Attenuation;
            break;
        default:
            MPM_THROW_INVALID_CODE_PATH();
        }
        _call_api_function(std::bind(func, mykonos_config.device, attenuation));
        break;
    }
    case RX_DIRECTION:
    {
        uint8_t gain = _convert_rx_gain_to_mykonos(value);
        coerced_value = static_cast<double>(gain);

        std::function<mykonosErr_t(mykonosDevice_t*, uint8_t)> func;
        switch (chain)
        {
        case chain_t::ONE:
            func = MYKONOS_setRx1ManualGain;
            break;
        case chain_t::TWO:
            func = MYKONOS_setRx2ManualGain;
            break;
        default:
            MPM_THROW_INVALID_CODE_PATH();
        }
        _call_api_function(std::bind(func, mykonos_config.device, gain));
        break;
    }
    default:
        MPM_THROW_INVALID_CODE_PATH();
    }

    _restore_from_config_state(state);
    // TODO: coerced_value here is wrong, should just call get_gain
    return coerced_value;
}

void ad937x_device::set_agc_mode(direction_t direction, gain_mode_t mode)
{
    mykonosGainMode_t mykonos_mode;
    switch (direction)
    {
    case RX_DIRECTION:
        switch (mode)
        {
        case gain_mode_t::MANUAL:
            mykonos_mode = MGC;
            break;
        case gain_mode_t::AUTOMATIC:
            mykonos_mode = AGC;
            break;
        case gain_mode_t::HYBRID:
            mykonos_mode = HYBRID;
            break;
        default:
            MPM_THROW_INVALID_CODE_PATH();
        }
        break;
    default:
        MPM_THROW_INVALID_CODE_PATH();
    }

    auto state = _move_to_config_state();
    _call_api_function(std::bind(MYKONOS_setRxGainControlMode, mykonos_config.device, mykonos_mode));
    _restore_from_config_state(state);
}

void ad937x_device::set_fir(
    const direction_t direction,
    const chain_t chain,
    int8_t gain,
    const std::vector<int16_t> & fir)
{
    switch (direction)
    {
    case TX_DIRECTION:
        mykonos_config.tx_fir_config.set_fir(gain, fir);
        break;
    case RX_DIRECTION:
        mykonos_config.rx_fir_config.set_fir(gain, fir);
        break;
    default:
        MPM_THROW_INVALID_CODE_PATH();
    }

    // TODO: reload this on device
}

void ad937x_device::set_gain_pin_step_sizes(direction_t direction, chain_t chain, double inc_step, double dec_step)
{
    if (direction == RX_DIRECTION)
    {
        gain_ctrl.config.at(direction).at(chain).inc_step = static_cast<uint8_t>(inc_step / 0.5);
        gain_ctrl.config.at(direction).at(chain).dec_step = static_cast<uint8_t>(dec_step / 0.5);
    }
    else if (direction == TX_DIRECTION) {
        // !!! TX is attenuation direction, so the pins are flipped !!!
        gain_ctrl.config.at(direction).at(chain).dec_step = static_cast<uint8_t>(inc_step / 0.05);
        gain_ctrl.config.at(direction).at(chain).inc_step = static_cast<uint8_t>(dec_step / 0.05);
    }
    else {
        MPM_THROW_INVALID_CODE_PATH();
    }
    _apply_gain_pins(direction, chain);
}

void ad937x_device::set_enable_gain_pins(direction_t direction, chain_t chain, bool enable)
{
    gain_ctrl.config.at(direction).at(chain).enable = enable;
    _apply_gain_pins(direction, chain);
}



/******************************************************
Get configuration functions
******************************************************/

double ad937x_device::get_freq(direction_t direction)
{
    mykonosRfPllName_t pll;
    switch (direction)
    {
    case TX_DIRECTION: pll = TX_PLL; break;
    case RX_DIRECTION: pll = RX_PLL; break;
    default:
        MPM_THROW_INVALID_CODE_PATH();
    }

    // TODO: coercion here causes extra device accesses, when the formula is provided on pg 119 of the user guide
    // Furthermore, because coerced is returned as an integer, it's not even accurate
    uint64_t coerced_pll;
    _call_api_function(std::bind(MYKONOS_getRfPllFrequency, mykonos_config.device, pll, &coerced_pll));
    return static_cast<double>(coerced_pll);
}

bool ad937x_device::get_pll_lock_status(uint8_t pll, bool wait_for_lock)
{
    uint8_t pll_status;
    _call_api_function(std::bind(MYKONOS_checkPllsLockStatus, mykonos_config.device, &pll_status));

    if (not wait_for_lock)
    {
        return (pll_status & pll) == pll;
    }
    else {
        auto lock_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(PLL_LOCK_TIMEOUT_MS);
        bool locked = false;
        while (not locked and lock_time > std::chrono::steady_clock::now())
        {
            locked = get_pll_lock_status(pll);
        }

        if (!locked)
        {
            // last chance
            locked = get_pll_lock_status(pll);
        }
        return locked;
    }
}

double ad937x_device::get_gain(direction_t direction, chain_t chain)
{
    switch (direction)
    {
        case TX_DIRECTION:
        {
            std::function<mykonosErr_t(mykonosDevice_t*, uint16_t*)> func;
            switch (chain)
            {
            case chain_t::ONE:
                func = MYKONOS_getTx1Attenuation;
                break;
            case chain_t::TWO:
                func = MYKONOS_getTx2Attenuation;
                break;
            }
            uint16_t atten;
            _call_api_function(std::bind(func, mykonos_config.device, &atten));
            return _convert_tx_gain_from_mykonos(atten);
        }
        case RX_DIRECTION:
        {
            std::function<mykonosErr_t(mykonosDevice_t*, uint8_t*)> func;
            switch (chain)
            {
            case chain_t::ONE:
                func = MYKONOS_getRx1Gain;
                break;
            case chain_t::TWO:
                func = MYKONOS_getRx2Gain;
                break;
            }
            uint8_t gain;
            _call_api_function(std::bind(func, mykonos_config.device, &gain));
            return _convert_rx_gain_from_mykonos(gain);
        }
        default:
            MPM_THROW_INVALID_CODE_PATH();
    }
}

std::vector<int16_t> ad937x_device::get_fir(
    const direction_t direction,
    const chain_t chain,
    int8_t &gain)
{
    switch (direction)
    {
    case TX_DIRECTION:
        return mykonos_config.tx_fir_config.get_fir(gain);
    case RX_DIRECTION:
        return mykonos_config.rx_fir_config.get_fir(gain);
    default:
        MPM_THROW_INVALID_CODE_PATH();
    }
}

int16_t ad937x_device::get_temperature()
{
    // TODO: deal with the status.tempValid flag
    mykonosTempSensorStatus_t status;
    _call_gpio_api_function(std::bind(MYKONOS_readTempSensor, mykonos_config.device, &status));
    return status.tempCode;
}
