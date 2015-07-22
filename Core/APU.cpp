#include "stdafx.h"
#include "../BlipBuffer/Blip_Buffer.h"
#include "APU.h"
#include "CPU.h"
#include "SquareChannel.h"
#include "TriangleChannel.h"
#include "NoiseChannel.h"
#include "DeltaModulationChannel.h"
#include "ApuFrameCounter.h"
#include "EmulationSettings.h"

APU* APU::Instance = nullptr;
IAudioDevice* APU::AudioDevice = nullptr;

APU::APU(MemoryManager* memoryManager)
{
	APU::Instance = this;

	_memoryManager = memoryManager;
	_blipBuffer.reset(new Blip_Buffer());
	_blipBuffer->sample_rate(APU::SampleRate);

	_outputBuffer = new int16_t[APU::SamplesPerFrame];

	_squareChannel.push_back(unique_ptr<SquareChannel>(new SquareChannel(AudioChannel::Square1, _blipBuffer.get(), true)));
	_squareChannel.push_back(unique_ptr<SquareChannel>(new SquareChannel(AudioChannel::Square2, _blipBuffer.get(), false)));
	_triangleChannel.reset(new TriangleChannel(AudioChannel::Triangle, _blipBuffer.get()));
	_noiseChannel.reset(new NoiseChannel(AudioChannel::Noise, _blipBuffer.get()));
	_deltaModulationChannel.reset(new DeltaModulationChannel(AudioChannel::DMC, _blipBuffer.get(), _memoryManager));
	_frameCounter.reset(new ApuFrameCounter(&APU::FrameCounterTick));

	_memoryManager->RegisterIODevice(_squareChannel[0].get());
	_memoryManager->RegisterIODevice(_squareChannel[1].get());
	_memoryManager->RegisterIODevice(_frameCounter.get());
	_memoryManager->RegisterIODevice(_triangleChannel.get());
	_memoryManager->RegisterIODevice(_noiseChannel.get());
	_memoryManager->RegisterIODevice(_deltaModulationChannel.get());

	Reset(false);
}

APU::~APU()
{
	delete[] _outputBuffer;
}

void APU::SetNesModel(NesModel model, bool forceInit)
{
	if(_nesModel != model || forceInit) {
		//Finish the current apu frame before switching model
		Run();

		_nesModel = model;
		_blipBuffer->clock_rate(model == NesModel::NTSC ? CPU::ClockRateNtsc : CPU::ClockRatePal);
		_squareChannel[0]->SetNesModel(model);
		_squareChannel[1]->SetNesModel(model);
		_triangleChannel->SetNesModel(model);
		_noiseChannel->SetNesModel(model);
		_deltaModulationChannel->SetNesModel(model);
		_frameCounter->SetNesModel(model);
	}
}

void APU::FrameCounterTick(FrameType type)
{
	//Quarter & half frame clock envelope & linear counter
	Instance->_squareChannel[0]->TickEnvelope();
	Instance->_squareChannel[1]->TickEnvelope();
	Instance->_triangleChannel->TickLinearCounter();
	Instance->_noiseChannel->TickEnvelope();

	if(type == FrameType::HalfFrame) {
		//Half frames clock length counter & sweep
		Instance->_squareChannel[0]->TickLengthCounter();
		Instance->_squareChannel[1]->TickLengthCounter();
		Instance->_triangleChannel->TickLengthCounter();
		Instance->_noiseChannel->TickLengthCounter();

		Instance->_squareChannel[0]->TickSweep();
		Instance->_squareChannel[1]->TickSweep();
	}
}

uint8_t APU::ReadRAM(uint16_t addr)
{
	//$4015 read
	Run();

	uint8_t status = 0;
	status |= _squareChannel[0]->GetStatus() ? 0x01 : 0x00;
	status |= _squareChannel[1]->GetStatus() ? 0x02 : 0x00;
	status |= _triangleChannel->GetStatus() ? 0x04 : 0x00;
	status |= _noiseChannel->GetStatus() ? 0x08 : 0x00;
	status |= _deltaModulationChannel->GetStatus() ? 0x10 : 0x00;
	status |= CPU::HasIRQSource(IRQSource::FrameCounter) ? 0x40 : 0x00;
	status |= CPU::HasIRQSource(IRQSource::DMC) ? 0x80 : 0x00;

	//Reading $4015 clears the Frame Counter interrupt flag.
	CPU::ClearIRQSource(IRQSource::FrameCounter);

	return status;
}

void APU::WriteRAM(uint16_t addr, uint8_t value)
{
	//$4015 write
	Run();

	//Writing to $4015 clears the DMC interrupt flag.
	//This needs to be done before setting the enabled flag for the DMC (because doing so can trigger an IRQ)
	CPU::ClearIRQSource(IRQSource::DMC);

	_squareChannel[0]->SetEnabled((value & 0x01) == 0x01);
	_squareChannel[1]->SetEnabled((value & 0x02) == 0x02);
	_triangleChannel->SetEnabled((value & 0x04) == 0x04);
	_noiseChannel->SetEnabled((value & 0x08) == 0x08);
	_deltaModulationChannel->SetEnabled((value & 0x10) == 0x10);
}

void APU::GetMemoryRanges(MemoryRanges &ranges)
{
	ranges.AddHandler(MemoryType::RAM, MemoryOperation::Read, 0x4015);
	ranges.AddHandler(MemoryType::RAM, MemoryOperation::Write, 0x4015);
}

void APU::Run()
{
	//Update framecounter and all channels
	//This is called:
	//-At the end of a frame
	//-Before APU registers are read/written to
	//-When a DMC or FrameCounter interrupt needs to be fired
	int32_t cyclesToRun = _currentCycle - _previousCycle;

	while(_previousCycle < _currentCycle) {
		_previousCycle += _frameCounter->Run(cyclesToRun);

		_squareChannel[0]->Run(_previousCycle);
		_squareChannel[1]->Run(_previousCycle);
		_noiseChannel->Run(_previousCycle);
		_triangleChannel->Run(_previousCycle);
		_deltaModulationChannel->Run(_previousCycle);
	}
}

void APU::StaticRun()
{
	Instance->Run();
}

bool APU::NeedToRun(uint32_t currentCycle)
{
	if(ApuLengthCounter::NeedToRun()) {
		return true;
	}

	uint32_t cyclesToRun = currentCycle - _previousCycle;
	if(_frameCounter->IrqPending(cyclesToRun)) {
		return true;
	} else if(_deltaModulationChannel->IrqPending(cyclesToRun)) {
		return true;
	}
	return false;
}

void APU::ExecStatic()
{
	Instance->Exec();
}

void APU::Exec()
{
	_currentCycle++;
	if(_currentCycle == 10000) {
		Run();

		_squareChannel[0]->EndFrame();
		_squareChannel[1]->EndFrame();
		_triangleChannel->EndFrame();
		_noiseChannel->EndFrame();
		_deltaModulationChannel->EndFrame();

		_blipBuffer->end_frame(_currentCycle);

		// Read some samples out of Blip_Buffer if there are enough to fill our output buffer
		size_t sampleCount = _blipBuffer->read_samples(_outputBuffer, APU::SamplesPerFrame);
		if(APU::AudioDevice) {
			APU::AudioDevice->PlayBuffer(_outputBuffer, (uint32_t)(sampleCount * BitsPerSample / 8));
		}
		_currentCycle = 0;
		_previousCycle = 0;
	} else if(NeedToRun(_currentCycle)) {
		Run();
	}
}

void APU::StopAudio()
{
	if(APU::AudioDevice) {
		APU::AudioDevice->Pause();
	}
}

void APU::Reset(bool softReset)
{
	_currentCycle = 0;
	_previousCycle = 0;
	_squareChannel[0]->Reset(softReset);
	_squareChannel[1]->Reset(softReset);
	_triangleChannel->Reset(softReset);
	_noiseChannel->Reset(softReset);
	_deltaModulationChannel->Reset(softReset);
	_frameCounter->Reset(softReset);
}

void APU::StreamState(bool saving)
{
	Stream<NesModel>(_nesModel);
	Stream<uint32_t>(_currentCycle);
	Stream<uint32_t>(_previousCycle);
	Stream(_squareChannel[0].get());
	Stream(_squareChannel[1].get());
	Stream(_triangleChannel.get());
	Stream(_noiseChannel.get());
	Stream(_deltaModulationChannel.get());
	Stream(_frameCounter.get());

	if(!saving) {
		SetNesModel(_nesModel, true);
	}
}