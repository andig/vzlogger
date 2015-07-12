/**
 * S0 Hutschienenzähler directly connected to an rs232 port
 *
 * @package vzlogger
 * @copyright Copyright (c) 2011, The volkszaehler.org project
 * @license http://www.gnu.org/licenses/gpl.txt GNU Public License
 * @author Steffen Vogel <info@steffenvogel.de>
 */
/*
 * This file is part of volkzaehler.org
 *
 * volkzaehler.org is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * volkzaehler.org is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with volkszaehler.org. If not, see <http://www.gnu.org/licenses/>.
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <poll.h>

#include "protocols/MeterS0.hpp"
#include "Options.hpp"
#include <VZException.hpp>

MeterS0::MeterS0(std::list<Option> options)
		: Protocol("s0")
		, _hwif(0)
		, _debounce_delay_ms(0)
		, _counter(0)
{
	OptionList optlist;

	// check which HWIF to use:
	// if "gpio" is given -> GPIO
	// else (assuming "device") -> UART
	bool use_gpio = false;

	try {
		int gpiopin = optlist.lookup_int(options, "gpio");
		if (gpiopin >=0 ) use_gpio = true;
	} catch (vz::VZException &e) {
			// ignore
	}

	if (use_gpio) {
		_hwif = new HWIF_GPIO(options);
	} else {
		_hwif = new HWIF_UART(options);
	}

	try {
		_resolution = optlist.lookup_int(options, "resolution");
	} catch (vz::OptionNotFoundException &e) {
		_resolution = 1000;
	} catch (vz::VZException &e) {
		print(log_error, "Failed to parse resolution", "");
		throw;
	}
	if (_resolution < 1) throw vz::VZException("Resolution must be greater than 0.");

	try {
		_debounce_delay_ms = optlist.lookup_int(options, "debounce_delay");
	} catch (vz::OptionNotFoundException &e) {
		_debounce_delay_ms = 30;
	} catch (vz::VZException &e) {
		print(log_error, "Failed to parse debounce_delay", "");
		throw;
	}
	if (_debounce_delay_ms < 0) throw vz::VZException("debounce_delay must not be negative.");

}

MeterS0::~MeterS0()
{
	if (_hwif) delete _hwif;
}

int MeterS0::open() {

	if (!_hwif) return ERR;

	if (!_hwif->_open()) return ERR;

	// have yet to wait for very first impulse
	_impulseReceived = false;

	return SUCCESS;
}

int MeterS0::close() {
	if (!_hwif) return ERR;

	return (_hwif->_close() ? SUCCESS : ERR);
}

ssize_t MeterS0::read(std::vector<Reading> &rds, size_t n) {

	struct timeval time_now;

	if (!_hwif) return 0;
	if (n<2) return 0; // would be worth a debug msg!
	bool neg = false;

	// wait for very first impulse
	if (!_impulseReceived) {
		if (!_hwif->waitForImpulse(neg)) return 0;

		gettimeofday(&_time_last, NULL);

		_impulseReceived = true;

		// store timestamp
		rds[0].identifier(new StringIdentifier(neg ? "Impulse_neg" : "Impulse"));
		rds[0].time(_time_last);
		rds[0].value(1);

		return 1;
	} else {
		// do we need to wait for debounce_delay?
		gettimeofday(&time_now, NULL);
		struct timeval delta;
		timersub(&time_now, &_time_last, &delta);
		long ms = (delta.tv_sec*1e3) + (delta.tv_usec / 1e3);
		if (ms<_debounce_delay_ms) {
			struct timespec ts;
			ms = _debounce_delay_ms - ms;
			print(log_finest, "Waiting %d ms for debouncing", name().c_str(), ms );
			ts.tv_sec = ms/1000;
			ts.tv_nsec = (ms%1000)*1e6;
			struct timespec rem;
			while ( (-1 == nanosleep(&ts, &rem)) && (errno == EINTR) ) {
				ts = rem;
			}
		}
	}

	if (!_hwif->waitForImpulse(neg)) return 0;

	gettimeofday(&time_now, NULL);

	// _time_last is initialized at this point
	double t1 = _time_last.tv_sec + _time_last.tv_usec / 1e6;
	double t2 = time_now.tv_sec + time_now.tv_usec / 1e6;
	double value = 3600000 / ((t2-t1) * _resolution);

	memcpy(&_time_last, &time_now, sizeof(struct timeval));

	// store current timestamp
	rds[0].identifier(new StringIdentifier(neg ? "Power_neg" : "Power")); // todo this is wrong if direction changes!
	rds[0].time(time_now);
	rds[0].value(value);

	rds[1].identifier(new StringIdentifier(neg ? "Impulse_neg" : "Impulse"));
	rds[1].time(time_now);
	rds[1].value(1);

	print(log_debug, "Reading S0 - n=%d power=%f dir=%s", name().c_str(), n, rds[0].value(), neg ? "-" : "+");

	return 2;
}

MeterS0::HWIF_UART::HWIF_UART(const std::list<Option> &options) :
	_fd(-1)
{
	OptionList optlist;

	try {
		_device = optlist.lookup_string(options, "device");
	} catch (vz::VZException &e) {
		print(log_error, "Missing device or invalid type", "");
		throw;
	}
}

MeterS0::HWIF_UART::~HWIF_UART()
{
	if (_fd>=0) _close();
}

bool MeterS0::HWIF_UART::_open()
{
	// open port
	int fd = ::open(_device.c_str(), O_RDWR | O_NOCTTY);

	if (fd < 0) {
		print(log_error, "open(%s): %s", "", _device.c_str(), strerror(errno));
		return false;
	}

	// save current port settings
	tcgetattr(fd, &_old_tio);

	// configure port
	struct termios tio;
	memset(&tio, 0, sizeof(struct termios));

	tio.c_cflag = B300 | CS8 | CLOCAL | CREAD;
	tio.c_iflag = IGNPAR;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	tio.c_cc[VMIN]=1;
	tio.c_cc[VTIME]=0;

	tcflush(fd, TCIFLUSH);

	// apply configuration
	tcsetattr(fd, TCSANOW, &tio);
	_fd = fd;

	return true;
}

bool MeterS0::HWIF_UART::_close()
{
	if (_fd<0) return false;

	tcsetattr(_fd, TCSANOW, &_old_tio); // reset serial port

	::close(_fd);
	_fd = -1;

	return true;
}

bool MeterS0::HWIF_UART::waitForImpulse(bool &neg)
{
	if (_fd<0) return false;
	char buf[8];

	// clear input buffer
	tcflush(_fd, TCIOFLUSH);

	// blocking until one character/pulse is read
	if (::read(_fd, buf, 8) < 1) return false;
	neg = false; // direction/sign not supported with UART HWIF
	return true;
}

MeterS0::HWIF_GPIO::HWIF_GPIO(const std::list<Option> &options) :
	_fd(-1), _fd_dir(-1), _gpiopin(-1), _gpio_dir_pin(-1), _configureGPIO(true)
{
	OptionList optlist;

	try {
		_gpiopin = optlist.lookup_int(options, "gpio");
	} catch (vz::VZException &e) {
		print(log_error, "Missing gpio or invalid type (expect int)", "S0");
		throw;
	}
	if (_gpiopin <0 ) throw vz::VZException("invalid (<0) gpio(pin) set");

	try {
		_configureGPIO = optlist.lookup_bool(options, "configureGPIO");
	} catch (vz::VZException &e) {
		print(log_info, "Missing bool configureGPIO using default true", "S0");
		_configureGPIO = true;
	}

	try {
		_gpio_dir_pin = optlist.lookup_int(options, "gpio_dir");
	} catch (vz::VZException &e) {
			// ignore, keep default, disabled.
	}
	if (_gpio_dir_pin == _gpiopin) throw vz::VZException("gpio_dir pin needs to be different than gpio pin");

	_device.append("/sys/class/gpio/gpio");
	_device.append(std::to_string(_gpiopin));
	_device.append("/value");

	if (_gpio_dir_pin >= 0) {
		_device_dir.append("/sys/class/gpio");
		_device_dir.append(std::to_string(_gpio_dir_pin));
		_device_dir.append("/value");
	}
}

MeterS0::HWIF_GPIO::~HWIF_GPIO()
{
	if (_fd>=0 || _fd_dir>=0) _close();
}

bool MeterS0::HWIF_GPIO::_open()
{
	std::string name;
	int fd;
	unsigned int res;

	// configure main gpio pin:
	if (!::access(_device.c_str(),F_OK)){
		// exists
	} else {
		if (_configureGPIO) {
			fd = ::open("/sys/class/gpio/export",O_WRONLY);
			if (fd<0) throw vz::VZException("open export failed");
			name.clear();
			name.append(std::to_string(_gpiopin));
			name.append("\n");

			res=write(fd,name.c_str(), name.length()+1); // is the trailing zero really needed?
			if ((name.length()+1)!=res) throw vz::VZException("export failed");
			::close(fd);
		} else return false; // doesn't exist and we shall not configure
	}

	// now it exists:
	if (_configureGPIO) {
		name.clear();
		name.append("/sys/class/gpio/gpio");
		name.append(std::to_string(_gpiopin));
		name.append("/direction");
		fd = ::open(name.c_str(), O_WRONLY);
		if (fd<0) throw vz::VZException("open direction failed");
		res=::write(fd,"in\n",3);
		if (3!=res) throw vz::VZException("set direction failed");
		if (::close(fd)<0) throw vz::VZException("set direction failed");

		name.clear();
		name.append("/sys/class/gpio/gpio");
		name.append(std::to_string(_gpiopin));
		name.append("/edge");
		fd = ::open(name.c_str(), O_WRONLY);
		if (fd<0) throw vz::VZException("open edge failed");
		res=::write(fd,"rising\n",7);
		if (7!=res) throw vz::VZException("set edge failed");
		if (::close(fd)<0) throw vz::VZException("set edge failed");

		name.clear();
		name.append("/sys/class/gpio/gpio");
		name.append(std::to_string(_gpiopin));
		name.append("/active_low");
		fd = ::open(name.c_str(), O_WRONLY);
		if (fd<0) throw vz::VZException("open active_low failed");
		res=::write(fd,"0\n",2);
		if (2!=res) throw vz::VZException("set active_low failed");
		if (::close(fd)<0) throw vz::VZException("set active_low failed");
	}

	fd = ::open( _device.c_str(), O_RDONLY | O_EXCL); // EXCL really needed?
	if (fd < 0) {
		print(log_error, "open(%s): %s", "", _device.c_str(), strerror(errno));
		return false;
	}

	_fd = fd;

	if (_gpio_dir_pin >= 0) {
		// configure direction gpio pin:
		if (!::access(_device_dir.c_str(),F_OK)){
			// exists
		} else {
			if (_configureGPIO) {
				fd = ::open("/sys/class/gpio/export",O_WRONLY);
				if (fd<0) throw vz::VZException("open export failed");
				name.clear();
				name.append(std::to_string(_gpio_dir_pin));
				name.append("\n");

				res=write(fd,name.c_str(), name.length()+1); // is the trailing zero really needed?
				if ((name.length()+1)!=res) throw vz::VZException("export gpio_dir pin failed");
				::close(fd);
			} else return false; // doesn't exist and we shall not configure
		}

		// now it exists:
		if (_configureGPIO) {
			name.clear();
			name.append("/sys/class/gpio/gpio");
			name.append(std::to_string(_gpio_dir_pin));
			name.append("/direction");
			fd = ::open(name.c_str(), O_WRONLY);
			if (fd<0) throw vz::VZException("open direction on gpio_dir pin failed");
			res=::write(fd,"in\n",3);
			if (3!=res) throw vz::VZException("set direction on gpio dir pin failed");
			if (::close(fd)<0) throw vz::VZException("set direction on gpio dir pin failed");

			// we configure for edge interrupt even though we don't use it for now
			name.clear();
			name.append("/sys/class/gpio/gpio");
			name.append(std::to_string(_gpio_dir_pin));
			name.append("/edge");
			fd = ::open(name.c_str(), O_WRONLY);
			if (fd<0) throw vz::VZException("open edge on gpio_dir pin failed");
			res=::write(fd,"rising\n",7);
			if (7!=res) throw vz::VZException("set edge on gpio_dir pin failed");
			if (::close(fd)<0) throw vz::VZException("set edge failed");

			name.clear();
			name.append("/sys/class/gpio/gpio");
			name.append(std::to_string(_gpio_dir_pin));
			name.append("/active_low");
			fd = ::open(name.c_str(), O_WRONLY);
			if (fd<0) throw vz::VZException("open active_low on gpio dir pin failed");
			res=::write(fd,"0\n",2);
			if (2!=res) throw vz::VZException("set active_low on gpio dir pin failed");
			if (::close(fd)<0) throw vz::VZException("set active_low failed");
		}

		fd = ::open( _device_dir.c_str(), O_RDONLY | O_EXCL); // EXCL really needed?
		if (fd < 0) {
			print(log_error, "open gpio dir pin (%s): %s", "", _device.c_str(), strerror(errno));
			return false;
		}

		_fd_dir = fd;
	}

	return true;
}

bool MeterS0::HWIF_GPIO::_close()
{
	if (_fd<0 && _fd_dir<0) return false;

	if (_fd >= 0) {
		::close(_fd);
		_fd = -1;
	}

	if (_fd_dir >= 0) {
		::close( _fd_dir );
		_fd_dir = -1;
	}

	return true;
}

bool MeterS0::HWIF_GPIO::waitForImpulse( bool &neg )
{
	unsigned char buf[2];
	if (_fd<0) return false;

	struct pollfd poll_fd;
	poll_fd.fd = _fd;
	poll_fd.events = POLLPRI|POLLERR;
	poll_fd.revents = 0;

	int rv = poll(&poll_fd, 1, -1);    // block endlessly
	print(log_debug, "MeterS0:HWIF_GPIO:first poll returned %d", "S0", rv);
	if (rv > 0) {
		if (poll_fd.revents & POLLPRI) {
			if (::pread(_fd, buf, 1, 0) < 1) return false;
			// now check direction:
			if (_gpio_dir_pin >= 0) {
				// if gpio dir pin set, direction is assumed to be negative:
				if (::pread(_fd_dir, buf, 1, 0) < 1) return false;
				if (buf[0] != '0') neg = true;
			} else neg = false;
		} else return false;
	} else return false;

	return true;
}
