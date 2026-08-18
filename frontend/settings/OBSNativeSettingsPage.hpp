/******************************************************************************
    Copyright (C) 2026 by OBS Studio Pro contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QString>

class QFormLayout;
class QLabel;
class QScrollArea;
class QVBoxLayout;
class QWidget;

class OBSNativeSettingsPage {
public:
	OBSNativeSettingsPage(QWidget *page, bool scrollable = true);

	QWidget *Contents() const { return contents; }
	QVBoxLayout *Layout() const { return layout; }
	QScrollArea *Scroll() const { return scroll; }

	static QScrollArea *MoveContentsToScroll(QWidget *page);
	static void ConfigureForm(QFormLayout *form);
	static QLabel *CreateLabel(QWidget *parent, const QString &text, QWidget *buddy = nullptr);
	static void AddRow(QFormLayout *form, QWidget *parent, const QString &text, QWidget *field);

private:
	QWidget *contents = nullptr;
	QVBoxLayout *layout = nullptr;
	QScrollArea *scroll = nullptr;
};
