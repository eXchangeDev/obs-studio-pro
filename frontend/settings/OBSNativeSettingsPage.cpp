/******************************************************************************
    Copyright (C) 2026 by OBS Studio Pro contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "OBSNativeSettingsPage.hpp"

#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace {

constexpr int NativeSettingsLabelWidth = 170;

void ReparentLayoutWidgets(QLayout *layout, QWidget *parent)
{
	if (!layout) {
		return;
	}

	for (int index = 0; index < layout->count(); ++index) {
		QLayoutItem *item = layout->itemAt(index);
		if (QWidget *widget = item->widget()) {
			widget->setParent(parent);
		} else if (QLayout *childLayout = item->layout()) {
			ReparentLayoutWidgets(childLayout, parent);
		}
	}
}

} // namespace

OBSNativeSettingsPage::OBSNativeSettingsPage(QWidget *page, bool scrollable)
{
	if (!page) {
		return;
	}

	QLayout *pageLayout = page->layout();
	if (!pageLayout) {
		layout = new QVBoxLayout(page);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(6);
	} else {
		layout = qobject_cast<QVBoxLayout *>(pageLayout);
	}

	if (!layout) {
		return;
	}

	if (!scrollable) {
		contents = page;
		return;
	}

	scroll = new QScrollArea(page);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);

	contents = new QWidget(scroll);
	auto *contentsLayout = new QVBoxLayout(contents);
	contentsLayout->setContentsMargins(0, 0, 0, 0);
	contentsLayout->setSpacing(6);
	contentsLayout->setSizeConstraint(QLayout::SetMinimumSize);
	contents->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	scroll->setWidget(contents);
	layout->addWidget(scroll);
	layout = contentsLayout;
}

QScrollArea *OBSNativeSettingsPage::MoveContentsToScroll(QWidget *page)
{
	if (!page || !page->layout()) {
		return nullptr;
	}

	QLayout *pageLayout = page->layout();
	auto *contents = new QWidget(page);
	auto *contentsLayout = new QVBoxLayout(contents);
	contentsLayout->setContentsMargins(0, 0, 0, 0);
	contentsLayout->setSpacing(pageLayout->spacing());
	contentsLayout->setSizeConstraint(QLayout::SetMinimumSize);
	contents->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

	while (QLayoutItem *item = pageLayout->takeAt(0)) {
		if (QWidget *widget = item->widget()) {
			widget->setParent(contents);
			contentsLayout->addWidget(widget);
			delete item;
		} else if (QLayout *childLayout = item->layout()) {
			ReparentLayoutWidgets(childLayout, contents);
			contentsLayout->addLayout(childLayout);
			delete item;
		} else {
			contentsLayout->addItem(item);
		}
	}

	auto *scroll = new QScrollArea(page);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	scroll->setWidget(contents);
	return scroll;
}

void OBSNativeSettingsPage::ConfigureForm(QFormLayout *form)
{
	if (!form) {
		return;
	}
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
	form->setContentsMargins(9, 2, 9, 9);
}

QLabel *OBSNativeSettingsPage::CreateLabel(QWidget *parent, const QString &text, QWidget *buddy)
{
	auto *label = new QLabel(text, parent);
	label->setMinimumSize(NativeSettingsLabelWidth, 0);
	label->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
	if (buddy) {
		label->setBuddy(buddy);
	}
	return label;
}

void OBSNativeSettingsPage::AddRow(QFormLayout *form, QWidget *parent, const QString &text, QWidget *field)
{
	if (form) {
		form->addRow(CreateLabel(parent, text, field), field);
	}
}
