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

#include <utility/OutputRoute.hpp>

#include <QDialog>

class OBSBasic;
class QPushButton;
class QTableWidget;

class OBSOutputRoutesDialog : public QDialog {
public:
	explicit OBSOutputRoutesDialog(OBSBasic *main);

private:
	OBSBasic *main;
	OBS::Output::RouteSet routes;
	QTableWidget *destinationTable = nullptr;
	QTableWidget *canvasTable = nullptr;
	QPushButton *editDestinationButton = nullptr;
	QPushButton *removeDestinationButton = nullptr;
	QPushButton *editCanvasButton = nullptr;
	QPushButton *removeCanvasButton = nullptr;

	void LoadRoutes();
	bool SaveRoutes();
	void RefreshDestinations();
	void RefreshCanvases();

	void AddDestination();
	void EditDestination();
	void RemoveDestination();
	void AddCanvas();
	void EditCanvas();
	void RemoveCanvas();
};
