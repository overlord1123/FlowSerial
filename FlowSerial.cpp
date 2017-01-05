/*
 * \		______ _                 _____            _                      _             
 * \\		|  ___| |               |  ___|          (_)                    (_)            
 * \\\		| |_  | | _____      __ | |__ _ __   __ _ _ _ __   ___  ___ _ __ _ _ __   __ _ 
 * \\\\		|  _| | |/ _ \ \ /\ / / |  __| '_ \ / _` | | '_ \ / _ \/ _ \ '__| | '_ \ / _` |
 * \\\\\	| |   | | (_) \ V  V /  | |__| | | | (_| | | | | |  __/  __/ |  | | | | | (_| |
 * \\\\\\	\_|   |_|\___/ \_/\_/   \____/_| |_|\__, |_|_| |_|\___|\___|_|  |_|_| |_|\__, |
 * \\\\\\\	                                     __/ |                                __/ |
 * \\\\\\\\                                     |___/                                |___/ 
 * \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\
 */
/** \file	FlwoSerial.cpp
 * \author		Jimmy van den Berg at Flow Engineering
 * \date		22-march-2016
 * \brief		A library for the pc to use the FlowSerial protocole.
 * \details 	FlowSerial was designed to send and recieve data ordendenly between devices.
 * 				It is peer based so communucation and behavior should be the same one both sides.
 * 				It was first designed and used for the AMD project in 2014.
 */

#include "FlowSerial.hpp"
#include <iostream>
#include <unistd.h>
#include <fcntl.h> //For the open parameter bits open(fd,"blabla", <this stuff>);
#include <termios.h> //For toptions and friends
#include <sys/select.h> //For the pselect command

//#define _DEBUG_FLOW_SERIAL_

using namespace std;

namespace FlowSerial{

	BaseSocket::BaseSocket(uint8_t* iflowRegister, size_t iregisterLenght):
		flowRegister(iflowRegister),
		registerLenght(iregisterLenght)
	{}

	bool BaseSocket::update(const uint8_t* const data, size_t arraySize){
		//by default return false. Return true when a frame has succesfully handled
		bool ret = false;
		for (uint i = 0; i < arraySize; ++i){
			uint8_t input = data[i];
			#ifdef _DEBUG_FLOW_SERIAL_
			cout << "next byte in line is number : " << +input << endl;
			#endif
			switch(flowSerialState){
				case State::idle:
					if(input == 0xAA){
						#ifdef _DEBUG_FLOW_SERIAL_
						cout << "Start recieved" << endl;
						#endif
						checksum = 0xAA;
						flowSerialState = State::startByteRecieved;
					}
					break;
				case State::startByteRecieved:
					#ifdef _DEBUG_FLOW_SERIAL_
					cout << "instructionRecieved" << endl;
					#endif
					instruction = static_cast<Instruction>(input);
					flowSerialState = State::instructionRecieved;
					checksum += input;
					argumentBytesRecieved = 0;
					break;
				case State::instructionRecieved:
					switch(instruction){
						case Instruction::read:
							if(argumentBytesRecieved == 0)
								startAddress = input;
							else{ //Pressumed 1
								nBytes = input;
								flowSerialState = State::argumentsRecieved;
							}
							break;
						case Instruction::write:
							if(argumentBytesRecieved == 0)
								startAddress = input;
							else if(argumentBytesRecieved == 1)
								nBytes = input;
							else{
								flowSerialBuffer[argumentBytesRecieved-2] = input;
								if(argumentBytesRecieved > nBytes)
									flowSerialState = State::argumentsRecieved;
							}
							break;
						case Instruction::returnRequestedData:
							if(argumentBytesRecieved == 0)
								nBytes = input;
							else{
								flowSerialBuffer[argumentBytesRecieved-1] = input;
								if(argumentBytesRecieved >= nBytes)
									flowSerialState = State::argumentsRecieved;
							}
					}
					#ifdef _DEBUG_FLOW_SERIAL_
					cout << "argumentBytesRecieved = " << argumentBytesRecieved << endl;
					#endif
					checksum += input;
					argumentBytesRecieved++;
					break;
				case State::argumentsRecieved:
					#ifdef _DEBUG_FLOW_SERIAL_
					cout << "MSB recieved" << endl;
					#endif
					checksumRecieved = input & 0xFF;
					flowSerialState = State::lsbChecksumRecieved;
					break;
				case State::lsbChecksumRecieved:
					#ifdef _DEBUG_FLOW_SERIAL_
					cout << "LSB recieved" << endl;
					#endif
					checksumRecieved |= (input << 8) & 0xFF00;
					if(checksum == checksumRecieved){
						flowSerialState = State::checksumOk;
						#ifdef _DEBUG_FLOW_SERIAL_
						cout << "checksum ok" << endl;
						#endif
						//No break. Continue to nex part.
					}
					else{
						//Message failed. return to idle state.
						#ifdef _DEBUG_FLOW_SERIAL_
						cout << "checksum not ok" << endl;
						cout << 
							"checksum:            " << checksum << '\n' <<
							"!= checksumRecieved: " << checksumRecieved << endl;
						#endif
						flowSerialState = State::idle;
						break;
					}
				case State::checksumOk:
					switch(instruction){
						case Instruction::read:
							#ifdef _DEBUG_FLOW_SERIAL_
							cout << "recieved Instruction::read request" << endl;
							#endif
							returnData(&FlowSerial::BaseSocket::flowRegister[startAddress], nBytes);
							break;
						case Instruction::write:
							#ifdef _DEBUG_FLOW_SERIAL_
							cout << "recieved Instruction::write request" << endl;
							#endif
							for (uint i = 0; i < nBytes; ++i){
								FlowSerial::BaseSocket::flowRegister[i + startAddress] = flowSerialBuffer[i];
							}
							break;
						case Instruction::returnRequestedData:
							#ifdef _DEBUG_FLOW_SERIAL_
							cout << "Got requested data" << endl;
							#endif
							inputBufferAvailable = nBytes;
							for (uint i = 0; i < nBytes; ++i){
								inputBuffer[i] = flowSerialBuffer[i];
							}
							inputBuffer[nBytes] = 0;
					}
					flowSerialState = State::idle;
					ret = true;
					break;
				default:
					flowSerialState = State::idle;
			}
		}
		return ret;
	}
	void BaseSocket::sendReadRequest(uint8_t startAddress, size_t nBytes){
		sendArray(startAddress, NULL, nBytes, Instruction::read);
	}
	void BaseSocket::writeToPeer(uint8_t startAddress, const uint8_t data[], size_t arraySize){
		sendArray(startAddress, data, arraySize, Instruction::write);
	}
	size_t BaseSocket::available(){
		return inputBufferAvailable;
	}
	void BaseSocket::clearReturnedData(){
		inputBufferAvailable = 0;
	}
	void BaseSocket::getReturnedData(uint8_t dataReturn[]){
		for (int i = 0; i < inputBufferAvailable; ++i){
			dataReturn[i] = inputBuffer[i];
		}
		inputBufferAvailable = 0;
	}
	void BaseSocket::returnData(const uint8_t data[], size_t arraySize){
		sendArray(0, data, arraySize, Instruction::returnRequestedData);
	}
	void BaseSocket::returnData(uint8_t data){
		sendArray(0, &data, 1, Instruction::returnRequestedData);
	}

	void BaseSocket::sendArray(uint8_t startAddress, const uint8_t data[], size_t arraySize, Instruction instruction){
		uint16_t checksum = 0xAA + static_cast<char>(instruction);
		size_t outIndex = 0;
		uint8_t charOut[1024];
		charOut[outIndex++] = 0xAA;
		charOut[outIndex++] = static_cast<uint>(instruction);
		if(instruction == Instruction::write || instruction == Instruction::read){
			charOut[outIndex++] = startAddress;
			checksum += startAddress;
		}
		charOut[outIndex++] = arraySize;
		checksum += arraySize;
		if(data != NULL){
			for (uint i = 0; i < arraySize; ++i){
				charOut[outIndex++] = data[i];
				checksum += data[i];
			}
		}
		charOut[outIndex++] = reinterpret_cast<const uint8_t*>(&checksum)[1];
		charOut[outIndex++] = reinterpret_cast<const uint8_t*>(&checksum)[0];
		sendToInterface(charOut, outIndex);
	}

	UsbSocket::UsbSocket(uint8_t* iflowRegister, size_t iregisterLenght)
		:BaseSocket(iflowRegister, iregisterLenght)
	{}

	UsbSocket::~UsbSocket()
	{
		closeDevice();
	}

	void UsbSocket::connectToDevice(const char filePath[], uint baudRate){
		if (fd < 0){
			fd = open(filePath, O_RDWR | O_NOCTTY);
			if (fd < 0){
				cerr << "Error: could not open device " << filePath << "." << endl;
				throw CouldNotOpenError();
			}
			#ifdef _DEBUG_FLOW_SERIAL_
			else{
				cout << "succesfully connected to UsbSocket " << filePath << endl;
			}
			#endif
			struct termios toptions;
			// Get current options of "Terminal" (since it is a tty i think)
			tcgetattr(fd, &toptions);
			// Adjust baud rate. see "man termios"
			switch(baudRate){
				case 0:
					cfsetspeed(&toptions, B0);
					break;
				case 50:
					cfsetspeed(&toptions, B50);
					break;
				case 75:
					cfsetspeed(&toptions, B75);
					break;
				case 110:
					cfsetspeed(&toptions, B110);
					break;
				case 134:
					cfsetspeed(&toptions, B134);
					break;
				case 150:
					cfsetspeed(&toptions, B150);
					break;
				case 200:
					cfsetspeed(&toptions, B200);
					break;
				case 300:
					cfsetspeed(&toptions, B300);
					break;
				case 600:
					cfsetspeed(&toptions, B600);
					break;
				case 1200:
					cfsetspeed(&toptions, B1200);
					break;
				case 1800:
					cfsetspeed(&toptions, B1800);
					break;
				case 2400:
					cfsetspeed(&toptions, B2400);
					break;
				case 4800:
					cfsetspeed(&toptions, B4800);
					break;
				case 9600:
					cfsetspeed(&toptions, B9600);
					break;
				case 19200:
					cfsetspeed(&toptions, B19200);
					break;
				case 38400:
					cfsetspeed(&toptions, B38400);
					break;
				case 57600:
					cfsetspeed(&toptions, B57600);
					break;
				case 115200:
					cfsetspeed(&toptions, B115200);
					break;
				case 230400:
					cfsetspeed(&toptions, B230400);
					break;
				default:
					cerr << "Warning: the set baud rate is not available.\nSelected 9600.\nPlease next time select one of the available following baud rates:\n" << 
					"0\n" <<
					"50\n" <<
					"75\n" <<
					"110\n" <<
					"134\n" <<
					"150\n" <<
					"200\n" <<
					"300\n" <<
					"600\n" <<
					"1200\n" <<
					"1800\n" <<
					"2400\n" <<
					"4800\n" <<
					"9600\n" <<
					"19200\n" <<
					"38400\n" <<
					"57600\n" <<
					"115200\n" <<
					"230400" << endl;
					cfsetspeed(&toptions, B9600);
			}

			// Adjust to 8 bits, no parity, no stop bits
			toptions.c_cflag &= ~PARENB;
			toptions.c_cflag &= ~CSTOPB;
			toptions.c_cflag &= ~CSIZE;
			toptions.c_cflag |= CS8;

			// Non-canonical mode. Means do not wait for newline before sending it through.
			toptions.c_lflag &= ~ICANON;
			// commit the serial port settings
			tcsetattr(fd, TCSANOW, &toptions);
		}
		else{
			cerr << "Error: Trying opening " << filePath << " but fd >= 0 so it must be already open." << endl;
			throw CouldNotOpenError();
		}
	}

	void UsbSocket::closeDevice(){
		if(fd >= 0){
			if(close(fd) < 0){
				cerr << "Error: could not close the device." << endl;
			}
			else{
				fd = -1;
			}
		}
	}

	bool UsbSocket::update(){
		if(fd >= 0){
			//Configure time to wait for response
			const static struct timespec timeout = {0, 500000000};
			//A set with fd's to check for timeout. Only one fd used here.
			fd_set readDiscriptors;
			FD_ZERO(&readDiscriptors);
			FD_SET(fd, &readDiscriptors);
			#ifdef _DEBUG_FLOW_SERIAL_
			cout << "Serial is open." << endl;
			#endif
			uint8_t inputBuffer[256];
			int pselectReturnValue = pselect(fd + 1, &readDiscriptors, NULL, NULL, &timeout, NULL);
			if(pselectReturnValue == -1){
				cerr << "Error: FlowSerial USB connection error. pselect function had an error" << endl;
				throw ConnectionError();
			}
			#ifdef _DEBUG_FLOW_SERIAL_
			else if(pselectReturnValue){
				cout << "Debug: FlowSerial Recieved a message whitin timeout" << endl;
			}
			#endif
			else if (pselectReturnValue == 0){
				cerr << "Error: FlowSerial USB connection error. timeout reached." << endl;
				throw TimeoutError();
			}

			//Actual reading done here
			uint recievedBytes = read(fd, inputBuffer, sizeof(inputBuffer) * sizeof(inputBuffer[0]) );
			#ifdef _DEBUG_FLOW_SERIAL_
			cout << "recieved bytes = " << recievedBytes << endl;
			#endif
			uint arrayMax = sizeof(inputBuffer)/sizeof(*inputBuffer);
			if(recievedBytes > arrayMax)
				recievedBytes = arrayMax;
			return FlowSerial::BaseSocket::update(inputBuffer, recievedBytes);
		}
		#ifdef _DEBUG_FLOW_SERIAL_
		cerr << "Error: Could not read from device/file. File is closed. fd < 0" << endl;
		#endif
		throw ReadError();
		return false;
	}

	void UsbSocket::sendToInterface(const uint8_t data[], size_t arraySize){
		#ifdef _DEBUG_FLOW_SERIAL_
		for (size_t i = 0; i < arraySize; ++i)
		{
			cout << "Writting to FlowSerial peer:" << +data[i] << endl;
		}
		#endif
		if(write(fd, data, arraySize) < 0){
			cerr << "Error: could not write to device/file" << endl;
			throw WriteError();
		}
	}

	bool UsbSocket::is_open(){
		return fd > 0;
	}

	void UsbSocket::readFromPeerAddress(uint8_t startAddress, uint8_t returnData[], size_t nBytes){
		sendReadRequest(startAddress, nBytes);
		// Wait for the data to be reached
		int trials = 0;
		while(available() < nBytes){
			try{
				update();
			}
			catch (TimeoutError){
				if(trials < 5){
					//Indacates error
					//Reset input data
					clearReturnedData();
					//Send another read request
					sendReadRequest(startAddress, nBytes);
					trials++;
				}
				else{
					throw ReadError();
				}
			}
		}
		getReturnedData(returnData);
	}
//end namespace FlowSerial
}
