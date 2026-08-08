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

#include "OBSOutputRoutes.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs-frontend-api.h>
#include <qt-wrappers.hpp>
#include <util/config-file.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>
#include <optional>

namespace {

constexpr int RouteIdRole = Qt::UserRole;
constexpr int DestinationIdRole = Qt::UserRole + 1;
constexpr int CanvasUuidRole = Qt::UserRole;
constexpr int CanvasNameRole = Qt::UserRole + 1;
constexpr int MainCanvasRole = Qt::UserRole + 2;

std::string ToStdString(const QString &value)
{
	const QByteArray utf8 = value.toUtf8();
	return {utf8.constData(), static_cast<size_t>(utf8.size())};
}

QString NewId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool ReferencesCanvas(const OBS::Output::CanvasReference &left, const OBS::Output::CanvasReference &right)
{
	if (!left.uuid.empty() && !right.uuid.empty()) {
		return left.uuid == right.uuid;
	}
	return !left.name.empty() && left.name == right.name;
}

void AddCanvasItem(QComboBox *combo, obs_canvas_t *canvas)
{
	if (!canvas) {
		return;
	}
	const char *name = obs_canvas_get_name(canvas);
	const char *uuid = obs_canvas_get_uuid(canvas);
	combo->addItem(QString::fromUtf8(name ? name : ""), QString::fromUtf8(uuid ? uuid : ""));
	const int index = combo->count() - 1;
	combo->setItemData(index, QString::fromUtf8(name ? name : ""), CanvasNameRole);
}

struct DestinationEdit {
	OBS::Output::CanvasReference canvas;
	OBS::Output::Destination destination;
};

std::optional<DestinationEdit> EditDestination(QWidget *parent, OBSBasic *main,
					       const OBS::Output::CanvasReference &initialCanvas,
					       const OBS::Output::Destination &initialDestination)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(QTStr("OBSPro.OutputRoutes.DestinationEditor"));
	auto *layout = new QVBoxLayout(&dialog);
	auto *form = new QFormLayout();
	layout->addLayout(form);

	auto *enabled = new QCheckBox(QTStr("OBSPro.OutputRoutes.Enabled"), &dialog);
	enabled->setChecked(initialDestination.enabled);
	form->addRow(QString(), enabled);

	auto *name = new QLineEdit(QString::fromUtf8(initialDestination.name.c_str()), &dialog);
	form->addRow(QTStr("OBSPro.OutputRoutes.Name"), name);

	auto *canvas = new QComboBox(&dialog);
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	AddCanvasItem(canvas, mainCanvas);
	for (const OBS::Canvas &additionalCanvas : main->GetCanvases()) {
		AddCanvasItem(canvas, additionalCanvas);
	}
	form->addRow(QTStr("OBSPro.OutputRoutes.Canvas"), canvas);

	for (int index = 0; index < canvas->count(); ++index) {
		const std::string uuid = ToStdString(canvas->itemData(index).toString());
		const std::string canvasName = ToStdString(canvas->itemData(index, CanvasNameRole).toString());
		if ((!initialCanvas.uuid.empty() && initialCanvas.uuid == uuid) ||
		    (initialCanvas.uuid.empty() && initialCanvas.name == canvasName)) {
			canvas->setCurrentIndex(index);
			break;
		}
	}

	auto *server = new QLineEdit(QString::fromUtf8(initialDestination.server.c_str()), &dialog);
	server->setPlaceholderText(QStringLiteral("rtmps://example.invalid/app"));
	form->addRow(QTStr("OBSPro.OutputRoutes.Server"), server);

	auto *streamKey = new QLineEdit(QString::fromUtf8(initialDestination.streamKey.c_str()), &dialog);
	streamKey->setEchoMode(QLineEdit::PasswordEchoOnEdit);
	form->addRow(QTStr("OBSPro.OutputRoutes.StreamKey"), streamKey);

	auto *useAuthentication = new QCheckBox(QTStr("OBSPro.OutputRoutes.UseAuthentication"), &dialog);
	useAuthentication->setChecked(initialDestination.useAuthentication);
	form->addRow(QString(), useAuthentication);

	auto *username = new QLineEdit(QString::fromUtf8(initialDestination.username.c_str()), &dialog);
	auto *password = new QLineEdit(QString::fromUtf8(initialDestination.password.c_str()), &dialog);
	password->setEchoMode(QLineEdit::PasswordEchoOnEdit);
	username->setEnabled(useAuthentication->isChecked());
	password->setEnabled(useAuthentication->isChecked());
	form->addRow(QTStr("OBSPro.OutputRoutes.Username"), username);
	form->addRow(QTStr("OBSPro.OutputRoutes.Password"), password);
	QObject::connect(useAuthentication, &QCheckBox::toggled, username, &QLineEdit::setEnabled);
	QObject::connect(useAuthentication, &QCheckBox::toggled, password, &QLineEdit::setEnabled);

	auto *priority = new QSpinBox(&dialog);
	priority->setRange(0, 999);
	priority->setValue(static_cast<int>(initialDestination.priority));
	form->addRow(QTStr("OBSPro.OutputRoutes.Priority"), priority);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addWidget(buttons);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
		if (name->text().trimmed().isEmpty() || server->text().trimmed().isEmpty()) {
			QMessageBox::warning(&dialog, QTStr("OBSPro.OutputRoutes.InvalidDestination"),
					     QTStr("OBSPro.OutputRoutes.NameServerRequired"));
			return;
		}
		dialog.accept();
	});

	if (dialog.exec() != QDialog::Accepted) {
		return std::nullopt;
	}

	DestinationEdit result;
	result.canvas.uuid = ToStdString(canvas->currentData().toString());
	result.canvas.name = ToStdString(canvas->currentData(CanvasNameRole).toString());
	result.destination = initialDestination;
	result.destination.name = ToStdString(name->text().trimmed());
	result.destination.service = "rtmp_custom";
	result.destination.server = ToStdString(server->text().trimmed());
	result.destination.streamKey = ToStdString(streamKey->text().trimmed());
	result.destination.useAuthentication = useAuthentication->isChecked();
	result.destination.username = ToStdString(username->text());
	result.destination.password = ToStdString(password->text());
	result.destination.priority = static_cast<uint32_t>(priority->value());
	result.destination.enabled = enabled->isChecked();
	return result;
}

struct CanvasEdit {
	QString name;
	uint32_t width = 1920;
	uint32_t height = 1080;
};

std::optional<CanvasEdit> EditCanvasProperties(QWidget *parent, const CanvasEdit &initial)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(QTStr("OBSPro.OutputRoutes.CanvasEditor"));
	auto *layout = new QVBoxLayout(&dialog);
	auto *form = new QFormLayout();
	layout->addLayout(form);

	auto *name = new QLineEdit(initial.name, &dialog);
	form->addRow(QTStr("OBSPro.OutputRoutes.Name"), name);

	auto *width = new QSpinBox(&dialog);
	width->setRange(64, 16384);
	width->setValue(static_cast<int>(initial.width));
	form->addRow(QTStr("OBSPro.OutputRoutes.Width"), width);

	auto *height = new QSpinBox(&dialog);
	height->setRange(64, 16384);
	height->setValue(static_cast<int>(initial.height));
	form->addRow(QTStr("OBSPro.OutputRoutes.Height"), height);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addWidget(buttons);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
		if (name->text().trimmed().isEmpty()) {
			QMessageBox::warning(&dialog, QTStr("OBSPro.OutputRoutes.InvalidCanvas"),
					     QTStr("OBSPro.OutputRoutes.CanvasNameRequired"));
			return;
		}
		dialog.accept();
	});

	if (dialog.exec() != QDialog::Accepted) {
		return std::nullopt;
	}

	return CanvasEdit{name->text().trimmed(), static_cast<uint32_t>(width->value()),
			  static_cast<uint32_t>(height->value())};
}

bool OutputsAreActive()
{
	return obs_frontend_streaming_active() || obs_frontend_recording_active() ||
	       obs_frontend_replay_buffer_active();
}

} // namespace

OBSOutputRoutesDialog::OBSOutputRoutesDialog(OBSBasic *main_) : QDialog(main_), main(main_)
{
	setWindowTitle(QTStr("OBSPro.OutputRoutes.Title"));
	setMinimumSize(760, 520);
	LoadRoutes();

	auto *layout = new QVBoxLayout(this);
	auto *description = new QLabel(QTStr("OBSPro.OutputRoutes.Description"), this);
	description->setWordWrap(true);
	layout->addWidget(description);

	auto *tabs = new QTabWidget(this);
	layout->addWidget(tabs, 1);

	auto *destinationsTab = new QWidget(tabs);
	auto *destinationsLayout = new QVBoxLayout(destinationsTab);
	destinationTable = new QTableWidget(destinationsTab);
	destinationTable->setColumnCount(4);
	destinationTable->setHorizontalHeaderLabels(
		{QTStr("OBSPro.OutputRoutes.Enabled"), QTStr("OBSPro.OutputRoutes.Name"),
		 QTStr("OBSPro.OutputRoutes.Canvas"), QTStr("OBSPro.OutputRoutes.Server")});
	destinationTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	destinationTable->setSelectionMode(QAbstractItemView::SingleSelection);
	destinationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	destinationTable->verticalHeader()->setVisible(false);
	destinationTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	destinationTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	destinationTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	destinationTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
	destinationsLayout->addWidget(destinationTable, 1);

	auto *destinationButtons = new QDialogButtonBox(destinationsTab);
	auto *addDestinationButton = destinationButtons->addButton(QTStr("OBSPro.OutputRoutes.AddDestination"),
								   QDialogButtonBox::ActionRole);
	editDestinationButton =
		destinationButtons->addButton(QTStr("OBSPro.OutputRoutes.Edit"), QDialogButtonBox::ActionRole);
	removeDestinationButton =
		destinationButtons->addButton(QTStr("OBSPro.OutputRoutes.Remove"), QDialogButtonBox::ActionRole);
	destinationsLayout->addWidget(destinationButtons);
	tabs->addTab(destinationsTab, QTStr("OBSPro.OutputRoutes.Destinations"));

	connect(addDestinationButton, &QPushButton::clicked, this, &OBSOutputRoutesDialog::AddDestination);
	connect(editDestinationButton, &QPushButton::clicked, this, &OBSOutputRoutesDialog::EditDestination);
	connect(removeDestinationButton, &QPushButton::clicked, this, &OBSOutputRoutesDialog::RemoveDestination);
	connect(destinationTable, &QTableWidget::itemDoubleClicked, this,
		[this](QTableWidgetItem *) { EditDestination(); });
	connect(destinationTable, &QTableWidget::itemSelectionChanged, this, [this]() {
		const bool selected = destinationTable->currentRow() >= 0;
		editDestinationButton->setEnabled(selected);
		removeDestinationButton->setEnabled(selected);
	});

	auto *canvasesTab = new QWidget(tabs);
	auto *canvasesLayout = new QVBoxLayout(canvasesTab);
	canvasTable = new QTableWidget(canvasesTab);
	canvasTable->setColumnCount(3);
	canvasTable->setHorizontalHeaderLabels({QTStr("OBSPro.OutputRoutes.Name"),
						QTStr("OBSPro.OutputRoutes.Resolution"),
						QTStr("OBSPro.OutputRoutes.Identifier")});
	canvasTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	canvasTable->setSelectionMode(QAbstractItemView::SingleSelection);
	canvasTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	canvasTable->verticalHeader()->setVisible(false);
	canvasTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	canvasTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	canvasTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	canvasesLayout->addWidget(canvasTable, 1);

	auto *canvasButtons = new QDialogButtonBox(canvasesTab);
	auto *addCanvasButton =
		canvasButtons->addButton(QTStr("OBSPro.OutputRoutes.AddCanvas"), QDialogButtonBox::ActionRole);
	editCanvasButton = canvasButtons->addButton(QTStr("OBSPro.OutputRoutes.Edit"), QDialogButtonBox::ActionRole);
	removeCanvasButton =
		canvasButtons->addButton(QTStr("OBSPro.OutputRoutes.Remove"), QDialogButtonBox::ActionRole);
	canvasesLayout->addWidget(canvasButtons);
	tabs->addTab(canvasesTab, QTStr("OBSPro.OutputRoutes.Canvases"));

	connect(addCanvasButton, &QPushButton::clicked, this, &OBSOutputRoutesDialog::AddCanvas);
	connect(editCanvasButton, &QPushButton::clicked, this, &OBSOutputRoutesDialog::EditCanvas);
	connect(removeCanvasButton, &QPushButton::clicked, this, &OBSOutputRoutesDialog::RemoveCanvas);
	connect(canvasTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *) { EditCanvas(); });
	connect(canvasTable, &QTableWidget::itemSelectionChanged, this, [this]() {
		const int row = canvasTable->currentRow();
		const bool editable = row >= 0 && !canvasTable->item(row, 0)->data(MainCanvasRole).toBool();
		editCanvasButton->setEnabled(editable);
		removeCanvasButton->setEnabled(editable);
	});

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, this);
	layout->addWidget(buttons);
	connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, this, [this]() { SaveRoutes(); });
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	RefreshDestinations();
	RefreshCanvases();
}

void OBSOutputRoutesDialog::LoadRoutes()
{
	const char *serialized = config_get_string(main->Config(), "Stream1", "OutputRoutes");
	if (!serialized || !*serialized) {
		return;
	}

	std::string error;
	if (!OBS::Output::Deserialize(serialized, routes, error)) {
		QMessageBox::warning(
			this, QTStr("OBSPro.OutputRoutes.InvalidConfiguration"),
			QTStr("OBSPro.OutputRoutes.InvalidConfigurationText").arg(QString::fromUtf8(error.c_str())));
	}
}

bool OBSOutputRoutesDialog::SaveRoutes()
{
	const auto errors = OBS::Output::Validate(routes);
	if (!errors.empty()) {
		QMessageBox::warning(this, QTStr("OBSPro.OutputRoutes.InvalidConfiguration"),
				     QString::fromUtf8(errors.front().c_str()));
		return false;
	}

	const std::string serialized = OBS::Output::Serialize(routes);
	config_set_string(main->Config(), "Stream1", "OutputRoutes", serialized.c_str());
	config_save_safe(main->Config(), "tmp", nullptr);
	return true;
}

void OBSOutputRoutesDialog::RefreshDestinations()
{
	destinationTable->setRowCount(0);
	for (const auto &route : routes.routes) {
		for (const auto &destination : route.destinations) {
			const int row = destinationTable->rowCount();
			destinationTable->insertRow(row);
			auto *enabled = new QTableWidgetItem(destination.enabled ? QTStr("Yes") : QTStr("No"));
			enabled->setData(RouteIdRole, QString::fromUtf8(route.id.c_str()));
			enabled->setData(DestinationIdRole, QString::fromUtf8(destination.id.c_str()));
			destinationTable->setItem(row, 0, enabled);
			destinationTable->setItem(row, 1,
						  new QTableWidgetItem(QString::fromUtf8(destination.name.c_str())));
			destinationTable->setItem(row, 2,
						  new QTableWidgetItem(QString::fromUtf8(route.canvas.name.c_str())));
			destinationTable->setItem(row, 3,
						  new QTableWidgetItem(QString::fromUtf8(destination.server.c_str())));
		}
	}
	editDestinationButton->setEnabled(false);
	removeDestinationButton->setEnabled(false);
}

void OBSOutputRoutesDialog::RefreshCanvases()
{
	canvasTable->setRowCount(0);
	auto addCanvas = [this](obs_canvas_t *canvas, bool mainCanvas) {
		if (!canvas) {
			return;
		}
		obs_video_info info{};
		obs_canvas_get_video_info(canvas, &info);
		const int row = canvasTable->rowCount();
		canvasTable->insertRow(row);
		auto *name = new QTableWidgetItem(QString::fromUtf8(obs_canvas_get_name(canvas)));
		name->setData(CanvasUuidRole, QString::fromUtf8(obs_canvas_get_uuid(canvas)));
		name->setData(MainCanvasRole, mainCanvas);
		canvasTable->setItem(row, 0, name);
		canvasTable->setItem(
			row, 1,
			new QTableWidgetItem(QStringLiteral("%1 × %2").arg(info.output_width).arg(info.output_height)));
		canvasTable->setItem(row, 2, new QTableWidgetItem(QString::fromUtf8(obs_canvas_get_uuid(canvas))));
	};

	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	addCanvas(mainCanvas, true);
	for (const OBS::Canvas &canvas : main->GetCanvases()) {
		addCanvas(canvas, false);
	}
	editCanvasButton->setEnabled(false);
	removeCanvasButton->setEnabled(false);
}

void OBSOutputRoutesDialog::AddDestination()
{
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	OBS::Output::CanvasReference canvas = OBS::Output::CanvasReferenceFromCanvas(mainCanvas);
	OBS::Output::Destination destination;
	destination.id = ToStdString(NewId());
	destination.priority = static_cast<uint32_t>(destinationTable->rowCount());

	auto edited = ::EditDestination(this, main, canvas, destination);
	if (!edited) {
		return;
	}

	auto route = std::find_if(routes.routes.begin(), routes.routes.end(),
				  [&](const auto &item) { return ReferencesCanvas(item.canvas, edited->canvas); });
	if (route == routes.routes.end()) {
		OBS::Output::Route newRoute;
		newRoute.id = ToStdString(NewId());
		newRoute.name = edited->canvas.name;
		newRoute.canvas = edited->canvas;
		routes.routes.emplace_back(std::move(newRoute));
		route = std::prev(routes.routes.end());
	}
	route->destinations.emplace_back(std::move(edited->destination));
	RefreshDestinations();
}

void OBSOutputRoutesDialog::EditDestination()
{
	const int row = destinationTable->currentRow();
	if (row < 0) {
		return;
	}
	const std::string routeId = ToStdString(destinationTable->item(row, 0)->data(RouteIdRole).toString());
	const std::string destinationId =
		ToStdString(destinationTable->item(row, 0)->data(DestinationIdRole).toString());
	auto route = std::find_if(routes.routes.begin(), routes.routes.end(),
				  [&](const auto &item) { return item.id == routeId; });
	if (route == routes.routes.end()) {
		return;
	}
	auto destination = std::find_if(route->destinations.begin(), route->destinations.end(),
					[&](const auto &item) { return item.id == destinationId; });
	if (destination == route->destinations.end()) {
		return;
	}

	auto edited = ::EditDestination(this, main, route->canvas, *destination);
	if (!edited) {
		return;
	}

	if (ReferencesCanvas(route->canvas, edited->canvas)) {
		*destination = std::move(edited->destination);
	} else {
		OBS::Output::Destination moved = std::move(edited->destination);
		route->destinations.erase(destination);
		auto target = std::find_if(routes.routes.begin(), routes.routes.end(), [&](const auto &item) {
			return ReferencesCanvas(item.canvas, edited->canvas);
		});
		if (target == routes.routes.end()) {
			OBS::Output::Route newRoute;
			newRoute.id = ToStdString(NewId());
			newRoute.name = edited->canvas.name;
			newRoute.canvas = edited->canvas;
			routes.routes.emplace_back(std::move(newRoute));
			target = std::prev(routes.routes.end());
		}
		target->destinations.emplace_back(std::move(moved));
	}

	routes.routes.erase(std::remove_if(routes.routes.begin(), routes.routes.end(),
					   [](const auto &item) { return item.destinations.empty(); }),
			    routes.routes.end());
	RefreshDestinations();
}

void OBSOutputRoutesDialog::RemoveDestination()
{
	const int row = destinationTable->currentRow();
	if (row < 0) {
		return;
	}
	const std::string routeId = ToStdString(destinationTable->item(row, 0)->data(RouteIdRole).toString());
	const std::string destinationId =
		ToStdString(destinationTable->item(row, 0)->data(DestinationIdRole).toString());
	for (auto &route : routes.routes) {
		if (route.id != routeId) {
			continue;
		}
		route.destinations.erase(std::remove_if(route.destinations.begin(), route.destinations.end(),
							[&](const auto &item) { return item.id == destinationId; }),
					 route.destinations.end());
		break;
	}
	routes.routes.erase(std::remove_if(routes.routes.begin(), routes.routes.end(),
					   [](const auto &item) { return item.destinations.empty(); }),
			    routes.routes.end());
	RefreshDestinations();
}

void OBSOutputRoutesDialog::AddCanvas()
{
	if (OutputsAreActive()) {
		QMessageBox::information(this, QTStr("OBSPro.OutputRoutes.OutputActive"),
					 QTStr("OBSPro.OutputRoutes.StopOutputsFirst"));
		return;
	}

	CanvasEdit initial{QTStr("OBSPro.OutputRoutes.NewCanvas"), 1080, 1920};
	auto edited = EditCanvasProperties(this, initial);
	if (!edited) {
		return;
	}

	OBSCanvasAutoRelease existing = obs_get_canvas_by_name(ToStdString(edited->name).c_str());
	if (existing) {
		QMessageBox::warning(this, QTStr("OBSPro.OutputRoutes.InvalidCanvas"),
				     QTStr("OBSPro.OutputRoutes.DuplicateCanvasName"));
		return;
	}

	obs_video_info info{};
	if (!obs_get_video_info(&info)) {
		return;
	}
	info.base_width = edited->width;
	info.base_height = edited->height;
	info.output_width = edited->width;
	info.output_height = edited->height;
	main->AddCanvas(ToStdString(edited->name), &info);
	main->SaveProject();
	RefreshCanvases();
}

void OBSOutputRoutesDialog::EditCanvas()
{
	const int row = canvasTable->currentRow();
	if (row < 0 || canvasTable->item(row, 0)->data(MainCanvasRole).toBool()) {
		return;
	}
	if (OutputsAreActive()) {
		QMessageBox::information(this, QTStr("OBSPro.OutputRoutes.OutputActive"),
					 QTStr("OBSPro.OutputRoutes.StopOutputsFirst"));
		return;
	}

	const std::string uuid = ToStdString(canvasTable->item(row, 0)->data(CanvasUuidRole).toString());
	OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(uuid.c_str());
	if (!canvas) {
		return;
	}
	obs_video_info info{};
	if (!obs_canvas_get_video_info(canvas, &info)) {
		return;
	}
	CanvasEdit initial{QString::fromUtf8(obs_canvas_get_name(canvas)), info.output_width, info.output_height};
	auto edited = EditCanvasProperties(this, initial);
	if (!edited) {
		return;
	}

	const std::string name = ToStdString(edited->name);
	obs_canvas_set_name(canvas, name.c_str());
	info.base_width = edited->width;
	info.base_height = edited->height;
	info.output_width = edited->width;
	info.output_height = edited->height;
	if (!obs_canvas_reset_video(canvas, &info)) {
		QMessageBox::warning(this, QTStr("OBSPro.OutputRoutes.InvalidCanvas"),
				     QTStr("OBSPro.OutputRoutes.CanvasResetFailed"));
		return;
	}
	for (auto &route : routes.routes) {
		if (route.canvas.uuid == uuid) {
			route.canvas.name = name;
			route.name = name;
		}
	}
	main->SaveProject();
	RefreshCanvases();
	RefreshDestinations();
}

void OBSOutputRoutesDialog::RemoveCanvas()
{
	const int row = canvasTable->currentRow();
	if (row < 0 || canvasTable->item(row, 0)->data(MainCanvasRole).toBool()) {
		return;
	}
	if (OutputsAreActive()) {
		QMessageBox::information(this, QTStr("OBSPro.OutputRoutes.OutputActive"),
					 QTStr("OBSPro.OutputRoutes.StopOutputsFirst"));
		return;
	}

	const std::string uuid = ToStdString(canvasTable->item(row, 0)->data(CanvasUuidRole).toString());
	const bool referenced = std::any_of(routes.routes.begin(), routes.routes.end(),
					    [&](const auto &route) { return route.canvas.uuid == uuid; });
	if (referenced) {
		QMessageBox::warning(this, QTStr("OBSPro.OutputRoutes.CanvasInUse"),
				     QTStr("OBSPro.OutputRoutes.RemoveDestinationsFirst"));
		return;
	}

	if (QMessageBox::question(this, QTStr("OBSPro.OutputRoutes.RemoveCanvas"),
				  QTStr("OBSPro.OutputRoutes.RemoveCanvasConfirm")) != QMessageBox::Yes) {
		return;
	}

	OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(uuid.c_str());
	if (!canvas) {
		return;
	}
	main->RemoveCanvas(OBSCanvas(canvas));
	main->SaveProject();
	RefreshCanvases();
}
