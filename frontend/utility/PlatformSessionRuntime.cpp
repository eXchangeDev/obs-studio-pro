/******************************************************************************
    Copyright (C) 2026 by OBS Studio Pro contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "PlatformSession.hpp"

#include "MultitrackVideoOutput.hpp"

#include <utility>

namespace OBS::Output {

PlatformSession::PlatformSession(PlatformSessionConfig config_) : config(std::move(config_)) {}

PlatformSession::~PlatformSession() = default;

::MultitrackVideoOutput &PlatformSession::EnsureMultitrackOutput()
{
	if (!multitrackOutput) {
		multitrackOutput = std::make_unique<::MultitrackVideoOutput>();
	}
	return *multitrackOutput;
}

void PlatformSession::ResetMultitrackOutput()
{
	DisconnectOutputSignals();
	output = nullptr;
	multitrackOutput.reset();
}

void PlatformSession::DisconnectOutputSignals()
{
	startingSignal.Disconnect();
	stoppingSignal.Disconnect();
	startedSignal.Disconnect();
	stoppedSignal.Disconnect();
}

} // namespace OBS::Output
