"""
open-dobot driver.

Implements driver to open firmware that controls Dobot FPGA.
Abstracts communication protocol, CCITT CRC and commands sent to FPGA.
Find firmware and SDK at https://github.com/maxosprojects/open-dobot

Author: maxosprojects (March 18 2016)
Additional Authors: <put your name here>

Version: 0.5.0

License: MIT
"""

import serial
import threading
import time
from serial import SerialException
import math

_max_trys = 1

CMD_READY = 0
CMD_STEPS = 1
CMD_EXEC_QUEUE = 2
CMD_GET_ACCELS = 3
CMD_SWITCH_TO_ACCEL_REPORT_MODE = 4
CMD_CALIBRATE_JOINT = 5
CMD_EMERGENCY_STOP = 6

piToDegrees = 180.0 / math.pi
halfPi = math.pi / 2.0


class DobotDriver:
	def __init__(self, comport, rate=115200):
		self._lock = threading.Lock()
		self._comport = comport
		self._rate = rate
		self._port = None
		self._crc = 0xffff

	def Open(self, timeout=0.025):
		try:
			self._port = serial.Serial(self._comport, baudrate=self._rate, timeout=timeout, interCharTimeout=0.01)
			# Have to wait for Arduino initialization to finish, or else it doesn't boot.
			time.sleep(2)
		except SerialException as e:
			print e
			exit(1)

	def Close(self):
		self._port.close()

	def _crc_clear(self):
		self._crc = 0xffff

	def _crc_update(self, data):
		self._crc = self._crc ^ (data << 8)
		for bit in range(0, 8):
			if (self._crc&0x8000) == 0x8000:
				self._crc = ((self._crc << 1) ^ 0x1021)
			else:
				self._crc = self._crc << 1

	def _readchecksumword(self):
		data = self._port.read(2)
		if len(data)==2:
			crc = (ord(data[0])<<8) | ord(data[1])
			return (1,crc)	
		return (0,0)

	def _readbyte(self):
		data = self._port.read(1)
		if len(data):
			val = ord(data)
			self._crc_update(val)
			return (1,val)	
		return (0,0)

	def _readword(self):
		val1 = self._readbyte()
		if val1[0]:
			val2 = self._readbyte()
			if val2[0]:
				return (1,val1[1]<<8|val2[1])
		return (0,0)

	def _readlong(self):
		val1 = self._readbyte()
		if val1[0]:
			val2 = self._readbyte()
			if val2[0]:
				val3 = self._readbyte()
				if val3[0]:
					val4 = self._readbyte()
					if val4[0]:
						return (1,val1[1]<<24|val2[1]<<16|val3[1]<<8|val4[1])
		return (0,0)	

	def _readslong(self):
		val = self._readlong()
		if val[0]:
			if val[1]&0x80000000:
				return (val[0],val[1]-0x100000000)
			return (val[0],val[1])
		return (0,0)

	def _read1(self, cmd):
		trys = _max_trys
		while trys:
			self._port.flushInput()
			self._sendcommand(cmd)
			self._writechecksum()
			val1 = self._readbyte()
			if val1[0]:
				crc = self._readchecksumword()
				if crc[0]:
					if self._crc&0xFFFF!=crc[1]&0xFFFF:
						# raise Exception('crc differs', self._crc, crc)
						return (0,0)
					return (1,val1[1])
			trys -= 1
		# raise Exception("couldn't get response in time for", _max_trys, 'times')
		return (0,0)

	def _read22(self, cmd):
		trys = _max_trys
		while trys:
			self._port.flushInput()
			self._sendcommand(cmd)
			self._writechecksum()
			val1 = self._readword()
			if val1[0]:
				val2 = self._readword()
				if val2[0]:
					crc = self._readchecksumword()
					if crc[0]:
						if self._crc&0xFFFF!=crc[1]&0xFFFF:
							# raise Exception('crc differs', self._crc, crc)
							return (0,0,0)
						return (1,val1[1],val2[1])
			trys -= 1
		# raise Exception("couldn't get response in time for", _max_trys, 'times')
		return (0,0,0)

	def _read4(self, cmd):
		trys = _max_trys
		while trys:
			self._port.flushInput()
			self._sendcommand(cmd)
			val1 = self._readlong()
			if val1[0]:
				crc = self._readchecksumword()
				if crc[0]:
					if self._crc&0xFFFF!=crc[1]&0xFFFF:
						return (0,0)
					return (1,val1[1])
			trys -= 1
		return (0,0)

	def _read4_1(self, cmd):
		trys = _max_trys
		while trys:
			self._port.flushInput()
			self._sendcommand(cmd)
			val1 = self._readslong()
			if val1[0]:
				val2 = self._readbyte()
				if val2[0]:
					crc = self._readchecksumword()
					if crc[0]:
						if self._crc&0xFFFF!=crc[1]&0xFFFF:
							return (0,0)
						return (1,val1[1],val2[1])
			trys -= 1
		return (0,0)

	def _writebyte(self, val):
		self._crc_update(val&0xFF)
		self._port.write(chr(val&0xFF))

	def _writeword(self, val):
		self._writebyte((val>>8)&0xFF)
		self._writebyte(val&0xFF)

	def _writelong(self, val):
		self._writebyte((val>>24)&0xFF)
		self._writebyte((val>>16)&0xFF)
		self._writebyte((val>>8)&0xFF)
		self._writebyte(val&0xFF)

	def _writechecksum(self):
		self._port.write(chr((self._crc>>8)&0xFF))
		self._port.write(chr(self._crc&0xFF))

	def _sendcommand(self, command):
		self._crc_clear()
		self._crc_update(command)
		self._port.write(chr(command))

	def _write(self, cmd, write_commands=list()):
		trys = _max_trys
		while trys:
			self._sendcommand(cmd)

			for c in write_commands:
				c[0](c[1])

			self._writechecksum()
			if self._readchecksumword():
				return True
			trys -= 1
		return False

	def _write0(self, cmd):
		return self._write(cmd)

	def _write1(self, cmd, val):
		return self._write(cmd, [(self._writebyte, val)])

	def _write2(self, cmd, val):
		return self._write(cmd, [(self._writeword, val)])

	def _write4(self, cmd, val):
		return self._write(cmd, [(self._writelong, val)])

	def _write14(self, cmd, val1, val2):
		return self._write(cmd, [(self._writebyte, val1), (self._writelong, val2)])

	def _write14411(self, cmd, val1, val2, val3, val4):
		return self._write(cmd, [(self._writelong, val1),
									(self._writelong, val2),
									(self._writebyte, val3),
									(self._writebyte, val4)])

	def _write_read(self, cmd, write_commands):
		tries = _max_trys
		while tries:
			self._port.flushInput()
			self._sendcommand(cmd)

			for c in write_commands:
				c[0](c[1])

			self._writechecksum()
			ret = self._readbyte()
			if ret[0]:
				crc = self._readchecksumword()
				if crc[0]:
					if self._crc & 0xFFFF != crc[1] & 0xFFFF:
						# raise Exception('crc differs', self._crc, crc)
						return (0, 0)
					return (1, ret[1])
			tries -= 1
		return (0, 0)

	def _write11121read1(self, cmd, val1, val2, val3, val4, val5):
		return self._write_read(cmd, [(self._writebyte, val1),
									(self._writebyte, val2),
									(self._writebyte, val3),
									(self._writeword, val4),
									(self._writebyte, val5)])

	def _write14441read1(self, cmd, val1, val2, val3, val4):
		return self._write_read(cmd, [(self._writelong, val1),
									(self._writelong, val2),
									(self._writelong, val3),
									(self._writebyte, val4)])

	def reverseBits32(self, val):
		# return long(bin(val&0xFFFFFFFF)[:1:-1], 2)
		return int('{0:032b}'.format(val)[::-1], 2)

	def freqToCmdVal(self, freq):
		'''
		Converts stepping frequency into a command value that dobot takes.
		'''
		if freq == 0:
			return 0x0242f000;
		return self.reverseBits32(long(25000000/freq))

	def stepsToCmdVal(self, steps):
		'''
		Converts number of steps for dobot to do in 20ms into a command value that dobot
		takes to set the stepping frequency.
		'''
		if steps == 0:
			return 0x0242f000;
		return self.reverseBits32(long(500000/steps))

	def accelToAngle(self, val, offset):
		return self.accelToRadians(val, offset) * piToDegrees

	def accelToRadians(self, val, offset):
		try:
			return math.asin(float(val - offset) / 493.56)
		except ValueError:
			return halfPi

	def CalibrateJoint(self, joint, forwardCommand, backwardCommand, direction, pin, pinMode, pullup):
		'''
		Initiates joint calibration procedure using a limit switch/photointerrupter. Effective immediately.
		Current command buffer is cleared.
		Cancel the procedure by issuing EmergencyStop() is necessary.
		
		@param joint - which joint to calibrate: 1-3
		@param forwardCommand - command to send to the joint when moving forward (towards limit switch);
				use freqToCmdVal()
		@param backwardCommand - command to send to the joint after hitting  (towards limit switch);
				use freqToCmdVal()
		@param direction - direction to move joint towards limit switch/photointerrupter: 0-1
		@param pin - firmware internal pin reference number that limit switch is connected to;
					refer to dobot.h -> calibrationPins
		@param pinMode - limit switch/photointerrupter normal LOW = 0, normal HIGH = 1
		@param pullup - enable pullup on the pin = 1, disable = 0
		@return True if command succesfully received, False otherwise.
		'''
		if 1 > joint > 3:
			return False
		control = ((pinMode & 0x01) << 4) | ((pullup & 0x01) << 3) | ((direction & 0x01) << 2) | ((joint - 1) & 0x03)
		self._lock.acquire()
		result = self._write14411(CMD_CALIBRATE_JOINT, forwardCommand, backwardCommand, pin, control)
		self._lock.release()
		return result

	def EmergencyStop(self):
		'''
		Stops the arm in case of emergency. Clears command buffer and cancels calibration procedure.

		@return True if command succesfully received, False otherwise.
		'''
		self._lock.acquire()
		result = self._write0(CMD_EMERGENCY_STOP)
		self._lock.release()
		return result

	def Steps(self, j1, j2, j3, j1dir, j2dir, j3dir, deferred=False):
		'''
		Adds a command to the controller's queue to execute on FPGA.
		@param j1 - joint1 subcommand
		@param j2 - joint2 subcommand
		@param j3 - joint3 subcommand
		@param j1dir - direction for joint1: 0-1
		@param j2dir - direction for joint2: 0-1
		@param j3dir - direction for joint3: 0-1
		@param deferred - defer execution of this command and all commands issued after this until
						the "ExecQueue" command is issued.
		@return Returns a tuple where the first element tells whether the command has been successfully
		received (0 - yes, 1 - timed out), and the second element tells whether the command was added
		to the controller's command queue (1 - added, 0 - not added, as the queue was full).
		'''
		control = ((j1dir & 0x01) << 7) | ((j2dir & 0x01) << 6) | ((j3dir & 0x01) << 5);
		# if deferred:
		# 	control |= 0x01
		self._lock.acquire()
		result = self._write14441read1(CMD_STEPS, j1, j2, j3, control)
		self._lock.release()
		return result

	def ExecQueue(self):
		'''
		Executes deferred commands.
		'''
		raise NotImplementedError()
		self._lock.acquire()
		result = self._write0(CMD_EXEC_QUEUE)
		self._lock.release()
		return result

	def GetAccelerometers(self):
		'''
		Returns data aquired from accelerometers at power on.
		There are 17 reads of each accelerometer that the firmware does and
		then averages the result before returning it here.
		'''
		self._lock.acquire()
		result = self._read22(CMD_GET_ACCELS)
		self._lock.release()
		return result

	def SwitchToAccelerometerReportMode(self):
		'''
		Apparently the following won't work because of the way dobot was desgined
		and limitations of AVR - cannot switch SPI from Slave to Master back.
		So, as a workaround, just hold the "Sensor Calibration" button and start your
		app. Arduino is reset on serial port connection and it takes about 2 seconds
		for it to start. After that you can release the button. That switches dobot to
		accelerometer reporting mode. To move the arm turn off the power switch.

		This function is left just in case a proper fix comes up.

		Switches dobot to accelerometer report mode.
		Dobot must be reset to enter normal mode after issuing this command.
		'''
		raise NotImplementedError('Read function description for more info')
		self._lock.acquire()
		result = self._write_read(CMD_SWITCH_TO_ACCEL_REPORT_MODE, [])
		self._lock.release()
		return result

	def isReady(self):
		'''
		Checks whether the controller is up and running.
		'''
		self._lock.acquire()
		result = self._read1(CMD_READY)
		self._lock.release()
		# Check for magic number.
		# return [result[0], result[1] == 0x40]
		return result

	def reset(self):
#		self._lock.acquire()
		i = 0
		while i < 5:
			self._port.flushInput()
			self._port.read(1)
			i += 1
		self._crc_clear()
#		self._lock.release()
