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

#pragma once

#include <functional>
#include <memory>

#include <QString>

class OBSBasic;
class QWidget;

// Integrates multi-destination streaming, encoded outputs, and canvases into
// OBS' existing Settings pages.  The class deliberately has no Q_OBJECT macro:
// all signal connections terminate in lambdas and the generated settings form
// remains the single owner of the native controls.
class OBSOutputRoutesSettings {
public:
	OBSOutputRoutesSettings(OBSBasic *main, QWidget *streamPage, QWidget *outputPage, QWidget *videoPage,
				std::function<void()> changedCallback);
	~OBSOutputRoutesSettings();

	OBSOutputRoutesSettings(const OBSOutputRoutesSettings &) = delete;
	OBSOutputRoutesSettings &operator=(const OBSOutputRoutesSettings &) = delete;

	void Load();
	bool Validate(QString &error);
	bool Save(QString &error);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
