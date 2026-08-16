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

#include <utility/PlatformSession.hpp>
#include <utility/OutputRoute.hpp>
#include <widgets/OBSBasic.hpp>

#include <obs-frontend-api.h>
#include <properties-view.hpp>
#include <qt-wrappers.hpp>
#include <util/config-file.h>

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QScreen>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QTabWidget>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <unordered_set>
#include <utility>

namespace {

using OBS::Output::CanvasReference;
using OBS::Output::Destination;
using OBS::Output::FailoverMode;
using OBS::Output::Route;
using OBS::Output::RouteSet;

std::string ToStdString(const QString &value)
{
	const QByteArray utf8 = value.toUtf8();
	return {utf8.constData(), static_cast<size_t>(utf8.size())};
}

QString NewId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString ResolutionText(uint32_t width, uint32_t height)
{
	return QStringLiteral("%1x%2").arg(width).arg(height);
}

bool ParseResolution(const QString &text, uint32_t &width, uint32_t &height)
{
	static const QRegularExpression expression(QStringLiteral("^\\s*(\\d{2,5})\\s*[xX]\\s*(\\d{2,5})\\s*$"));
	const QRegularExpressionMatch match = expression.match(text);
	if (!match.hasMatch()) {
		return false;
	}

	bool widthOk = false;
	bool heightOk = false;
	const uint parsedWidth = match.captured(1).toUInt(&widthOk);
	const uint parsedHeight = match.captured(2).toUInt(&heightOk);
	if (!widthOk || !heightOk || parsedWidth < 32 || parsedHeight < 32 || parsedWidth > 16384 ||
	    parsedHeight > 16384) {
		return false;
	}

	width = parsedWidth;
	height = parsedHeight;
	return true;
}

QString AspectRatioText(const QString &resolution)
{
	uint32_t width = 0;
	uint32_t height = 0;
	if (!ParseResolution(resolution, width, height)) {
		return {};
	}

	const uint32_t divisor = std::gcd(width, height);
	return QTStr("AspectRatio").arg(QString::number(width / divisor), QString::number(height / divisor));
}

void AddResolutionItem(QComboBox *combo, uint32_t width, uint32_t height)
{
	const QString resolution = ResolutionText(width, height);
	if (combo->findText(resolution) < 0) {
		combo->addItem(resolution);
	}
}

uint32_t ScaledResolution(uint32_t value, double scale, uint32_t alignment)
{
	const auto scaled = static_cast<uint32_t>(std::round(static_cast<double>(value) * scale));
	return std::max(32U, scaled & alignment);
}

void PopulateCanvasResolutionCombo(QComboBox *combo, const obs_video_info &info, bool baseResolution)
{
	combo->setEditable(true);
	combo->setDuplicatesEnabled(false);
	combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	combo->clear();

	if (baseResolution) {
		for (QScreen *screen : QGuiApplication::screens()) {
			const QSize size = screen->size();
			const auto width = static_cast<uint32_t>(std::round(size.width() * screen->devicePixelRatio()));
			const auto height =
				static_cast<uint32_t>(std::round(size.height() * screen->devicePixelRatio()));
			AddResolutionItem(combo, width, height);
		}
		AddResolutionItem(combo, 1920, 1080);
		AddResolutionItem(combo, 1280, 720);
	} else {
		static constexpr double scales[] = {1.0,       1.0 / 1.25, 1.0 / 0.75, 1.0 / 1.5,
						    1.0 / 0.6, 1.0 / 1.75, 1.0 / 2.0,  1.0 / 2.25,
						    1.0 / 2.5, 1.0 / 2.75, 1.0 / 3.0};
		for (const double scale : scales) {
			AddResolutionItem(combo, ScaledResolution(info.base_width, scale, ~1U),
					  ScaledResolution(info.base_height, scale, ~1U));
		}
	}

	combo->setCurrentText(ResolutionText(baseResolution ? info.base_width : info.output_width,
					     baseResolution ? info.base_height : info.output_height));
}

void UpdateAspectRatio(QComboBox *combo, QLabel *aspect)
{
	aspect->setText(AspectRatioText(combo->currentText()));
}

bool ParseCommonFps(const QString &text, uint32_t &numerator, uint32_t &denominator)
{
	if (text == QStringLiteral("24 NTSC")) {
		numerator = 24000;
		denominator = 1001;
		return true;
	}
	if (text == QStringLiteral("25 PAL")) {
		numerator = 25;
		denominator = 1;
		return true;
	}
	if (text == QStringLiteral("29.97")) {
		numerator = 30000;
		denominator = 1001;
		return true;
	}
	if (text == QStringLiteral("59.94")) {
		numerator = 60000;
		denominator = 1001;
		return true;
	}
	if (text == QStringLiteral("50 PAL")) {
		numerator = 50;
		denominator = 1;
		return true;
	}

	bool ok = false;
	const double value = text.toDouble(&ok);
	if (!ok || value <= 0.0) {
		return false;
	}
	numerator = static_cast<uint32_t>(std::round(value));
	denominator = 1;
	return numerator > 0;
}

void PopulateCanvasDownscaleFilter(QComboBox *combo, const obs_video_info &info, const QString &baseResolution,
				   const QString &outputResolution)
{
	QSignalBlocker blocker(combo);
	combo->clear();

	uint32_t baseWidth = 0;
	uint32_t baseHeight = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	const bool validResolutions = ParseResolution(baseResolution, baseWidth, baseHeight) &&
				      ParseResolution(outputResolution, outputWidth, outputHeight);
	if (validResolutions && baseWidth == outputWidth && baseHeight == outputHeight) {
		combo->addItem(QTStr("Basic.Settings.Video.DownscaleFilter.Unavailable"),
			       static_cast<int>(info.scale_type));
		combo->setEnabled(false);
		return;
	}

	combo->setEnabled(true);
	combo->addItem(QTStr("Basic.Settings.Video.DownscaleFilter.Bilinear"), static_cast<int>(OBS_SCALE_BILINEAR));
	combo->addItem(QTStr("Basic.Settings.Video.DownscaleFilter.Area"), static_cast<int>(OBS_SCALE_AREA));
	combo->addItem(QTStr("Basic.Settings.Video.DownscaleFilter.Bicubic"), static_cast<int>(OBS_SCALE_BICUBIC));
	combo->addItem(QTStr("Basic.Settings.Video.DownscaleFilter.Lanczos"), static_cast<int>(OBS_SCALE_LANCZOS));

	int scaleIndex = combo->findData(static_cast<int>(info.scale_type));
	if (scaleIndex < 0) {
		scaleIndex = combo->findData(static_cast<int>(OBS_SCALE_BICUBIC));
	}
	combo->setCurrentIndex(scaleIndex);
}

OBSDataAutoRelease SettingsFromJson(const std::string &serialized, obs_data_t *defaults)
{
	OBSDataAutoRelease settings = obs_data_newref(defaults);
	if (!serialized.empty()) {
		OBSDataAutoRelease saved = obs_data_create_from_json(serialized.c_str());
		if (saved) {
			obs_data_apply(settings, saved);
		}
	}
	return settings;
}

std::string SettingsJson(obs_data_t *settings)
{
	const char *json = settings ? obs_data_get_json(settings) : nullptr;
	return json ? json : "";
}

constexpr int NativeSettingsLabelWidth = 170;

void ConfigureNativeSettingsForm(QFormLayout *form)
{
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
	form->setContentsMargins(9, 2, 9, 9);
}

QLabel *CreateNativeSettingsLabel(QWidget *parent, const QString &text, QWidget *buddy = nullptr)
{
	auto *label = new QLabel(text, parent);
	label->setMinimumSize(NativeSettingsLabelWidth, 0);
	label->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
	if (buddy) {
		label->setBuddy(buddy);
	}
	return label;
}

void AddNativeSettingsRow(QFormLayout *form, QWidget *parent, const QString &text, QWidget *field)
{
	form->addRow(CreateNativeSettingsLabel(parent, text, field), field);
}

QVBoxLayout *CreateNativeSettingsPage(QWidget *page, QWidget *&contents, bool scrollable = true)
{
	auto *pageLayout = new QVBoxLayout(page);
	pageLayout->setContentsMargins(9, 0, 0, 0);
	pageLayout->setSpacing(6);

	if (!scrollable) {
		contents = page;
		return pageLayout;
	}

	auto *scroll = new QScrollArea(page);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);

	contents = new QWidget(scroll);
	auto *contentsLayout = new QVBoxLayout(contents);
	contentsLayout->setContentsMargins(0, 0, 0, 0);
	contentsLayout->setSpacing(6);
	scroll->setWidget(contents);
	pageLayout->addWidget(scroll);
	return contentsLayout;
}

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

QWidget *MovePageContentsToTab(QWidget *page, const QString &title, QTabWidget *&tabs)
{
	QLayout *pageLayout = page->layout();
	auto *nativePage = new QWidget(page);
	auto *nativeLayout = new QVBoxLayout(nativePage);
	nativeLayout->setContentsMargins(0, 0, 0, 0);
	nativeLayout->setSpacing(pageLayout->spacing());

	while (QLayoutItem *item = pageLayout->takeAt(0)) {
		if (QWidget *widget = item->widget()) {
			// QLayout::takeAt() transfers the layout item, but it does not
			// reparent the widget.  Leaving the native controls on the
			// original settings page makes them continue painting over the
			// dynamically-created tab pages.
			widget->setParent(nativePage);
			nativeLayout->addWidget(widget);
			delete item;
		} else if (QLayout *layout = item->layout()) {
			ReparentLayoutWidgets(layout, nativePage);
			nativeLayout->addLayout(layout);
			delete item;
		} else {
			nativeLayout->addItem(item);
		}
	}

	tabs = new QTabWidget(page);
	pageLayout->addWidget(tabs);
	tabs->addTab(nativePage, title);
	return nativePage;
}

QToolButton *AddCornerButton(QTabWidget *tabs, const QString &toolTip)
{
	auto *button = new QToolButton(tabs);
	button->setText(QStringLiteral("+"));
	button->setToolTip(toolTip);
	button->setAutoRaise(true);
	tabs->setCornerWidget(button, Qt::TopRightCorner);
	return button;
}

bool OutputsAreActive()
{
	return obs_frontend_streaming_active() || obs_frontend_recording_active() ||
	       obs_frontend_replay_buffer_active();
}

} // namespace

struct OBSOutputRoutesSettings::Impl {
	enum class CanvasCreationMode {
		Blank,
		ReuseSources,
		IndependentSources,
	};

	struct CanvasDraft {
		QString id;
		QString uuid;
		QString originalName;
		QString name;
		obs_video_info info{};
		bool existing = false;
		CanvasCreationMode creationMode = CanvasCreationMode::ReuseSources;
	};

	struct DestinationUi {
		QString id;
		QWidget *page = nullptr;
		QCheckBox *enabled = nullptr;
		QLineEdit *name = nullptr;
		QComboBox *output = nullptr;
		QComboBox *serviceType = nullptr;
		QSpinBox *priority = nullptr;
		QVBoxLayout *propertiesLayout = nullptr;
		OBSPropertiesView *properties = nullptr;
	};

	struct RouteUi {
		QString id;
		QWidget *page = nullptr;
		QCheckBox *enabled = nullptr;
		QLineEdit *name = nullptr;
		QComboBox *canvas = nullptr;
		QComboBox *videoEncoder = nullptr;
		QComboBox *audioEncoder = nullptr;
		QSpinBox *audioMix = nullptr;
		QComboBox *failoverMode = nullptr;
		QVBoxLayout *videoPropertiesLayout = nullptr;
		QVBoxLayout *audioPropertiesLayout = nullptr;
		OBSPropertiesView *videoProperties = nullptr;
		OBSPropertiesView *audioProperties = nullptr;
		QLabel *videoInheritanceNotice = nullptr;
		QLabel *audioInheritanceNotice = nullptr;
		QLabel *assignedDestinations = nullptr;
	};

	struct CanvasUi {
		QString id;
		QWidget *page = nullptr;
		QLineEdit *name = nullptr;
		QComboBox *baseResolution = nullptr;
		QLabel *baseAspect = nullptr;
		QComboBox *outputResolution = nullptr;
		QLabel *outputAspect = nullptr;
		QComboBox *downscaleFilter = nullptr;
		QComboBox *fpsType = nullptr;
		QStackedWidget *fpsTypes = nullptr;
		QComboBox *fpsCommon = nullptr;
		QSpinBox *fpsInteger = nullptr;
		QSpinBox *fpsNumerator = nullptr;
		QSpinBox *fpsDenominator = nullptr;
	};

	OBSBasic *main = nullptr;
	std::function<void()> changedCallback;
	bool loading = false;

	RouteSet routes;
	std::vector<CanvasDraft> canvasDrafts;
	std::set<QString> deletedCanvasUuids;

	QTabWidget *destinationTabs = nullptr;
	QTabWidget *outputTabs = nullptr;
	QTabWidget *canvasTabs = nullptr;
	QWidget *nativeStreamPage = nullptr;
	QWidget *nativeOutputPage = nullptr;
	QWidget *nativeVideoPage = nullptr;
	QComboBox *primaryOutput = nullptr;

	std::vector<std::unique_ptr<DestinationUi>> destinationUis;
	std::vector<std::unique_ptr<RouteUi>> routeUis;
	std::vector<std::unique_ptr<CanvasUi>> canvasUis;

	Impl(OBSBasic *main_, QWidget *streamPage, QWidget *outputPage, QWidget *videoPage,
	     std::function<void()> changed)
		: main(main_),
		  changedCallback(std::move(changed))
	{
		nativeStreamPage =
			MovePageContentsToTab(streamPage, QTStr("OBSPro.Settings.Stream.Primary"), destinationTabs);
		nativeOutputPage = MovePageContentsToTab(outputPage, QTStr("OBSPro.Settings.Output.Main"), outputTabs);
		nativeVideoPage = MovePageContentsToTab(videoPage, QTStr("OBSPro.Settings.Canvas.Main"), canvasTabs);

		auto *mapping = new QGroupBox(QTStr("OBSPro.Settings.Stream.Output"), nativeStreamPage);
		auto *mappingLayout = new QFormLayout(mapping);
		mappingLayout->setContentsMargins(9, 2, 9, 9);
		primaryOutput = new QComboBox(mapping);
		primaryOutput->addItem(QTStr("OBSPro.Settings.Output.Main"));
		primaryOutput->setEnabled(false);
		mappingLayout->addRow(QTStr("OBSPro.Settings.Stream.EncodedOutput"), primaryOutput);
		if (auto *layout = qobject_cast<QVBoxLayout *>(nativeStreamPage->layout())) {
			layout->insertWidget(0, mapping);
		}

		QToolButton *addDestination =
			AddCornerButton(destinationTabs, QTStr("OBSPro.Settings.Stream.AddDestination"));
		QObject::connect(addDestination, &QToolButton::clicked, destinationTabs,
				 [this]() { AddDestination(); });

		QToolButton *addOutput = AddCornerButton(outputTabs, QTStr("OBSPro.Settings.Output.Add"));
		QObject::connect(addOutput, &QToolButton::clicked, outputTabs, [this]() { AddOutput(); });

		QToolButton *addCanvas = AddCornerButton(canvasTabs, QTStr("OBSPro.Settings.Canvas.Add"));
		auto *canvasMenu = new QMenu(addCanvas);
		canvasMenu->addAction(QTStr("OBSPro.Settings.Canvas.AddBlank"), canvasTabs,
				      [this]() { AddCanvas(CanvasCreationMode::Blank); });
		canvasMenu->addAction(QTStr("OBSPro.Settings.Canvas.AddReuse"), canvasTabs,
				      [this]() { AddCanvas(CanvasCreationMode::ReuseSources); });
		canvasMenu->addAction(QTStr("OBSPro.Settings.Canvas.AddIndependent"), canvasTabs,
				      [this]() { AddCanvas(CanvasCreationMode::IndependentSources); });
		addCanvas->setMenu(canvasMenu);
		addCanvas->setPopupMode(QToolButton::InstantPopup);

		if (QComboBox *service = nativeStreamPage->findChild<QComboBox *>(QStringLiteral("service"))) {
			QObject::connect(service, &QComboBox::currentTextChanged, destinationTabs,
					 [this](const QString &text) {
						 if (!text.isEmpty()) {
							 destinationTabs->setTabText(0, text);
						 }
					 });
		}
	}

	void MarkChanged()
	{
		if (!loading && changedCallback) {
			changedCallback();
		}
	}

	Route *FindRoute(const QString &id)
	{
		auto found = std::find_if(routes.routes.begin(), routes.routes.end(),
					  [&](const Route &route) { return route.id == ToStdString(id); });
		return found == routes.routes.end() ? nullptr : &*found;
	}

	Route *PrimaryRoute()
	{
		auto found = std::find_if(routes.routes.begin(), routes.routes.end(),
					  [](const Route &route) { return route.primary; });
		return found == routes.routes.end() ? nullptr : &*found;
	}

	std::pair<Route *, Destination *> FindDestination(const QString &id)
	{
		const std::string destinationId = ToStdString(id);
		for (Route &route : routes.routes) {
			auto found = std::find_if(route.destinations.begin(), route.destinations.end(),
						  [&](const Destination &destination) {
							  return destination.id == destinationId;
						  });
			if (found != route.destinations.end()) {
				return {&route, &*found};
			}
		}
		return {nullptr, nullptr};
	}

	CanvasDraft *FindCanvasDraft(const QString &id)
	{
		auto found = std::find_if(canvasDrafts.begin(), canvasDrafts.end(),
					  [&](const CanvasDraft &draft) { return draft.id == id; });
		return found == canvasDrafts.end() ? nullptr : &*found;
	}

	CanvasReference CanvasReferenceForId(const QString &id) const
	{
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		if (id == QString::fromUtf8(obs_canvas_get_uuid(mainCanvas))) {
			return OBS::Output::CanvasReferenceFromCanvas(mainCanvas);
		}

		auto found = std::find_if(canvasDrafts.begin(), canvasDrafts.end(),
					  [&](const CanvasDraft &draft) { return draft.id == id; });
		if (found == canvasDrafts.end()) {
			return {};
		}
		CanvasReference reference;
		reference.uuid = ToStdString(found->uuid);
		reference.name = ToStdString(found->name);
		return reference;
	}

	QString CanvasIdForReference(const CanvasReference &reference) const
	{
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		if (OBS::Output::CanvasReferenceMatches(reference, mainCanvas)) {
			return QString::fromUtf8(obs_canvas_get_uuid(mainCanvas));
		}
		for (const CanvasDraft &draft : canvasDrafts) {
			if ((!reference.uuid.empty() && reference.uuid == ToStdString(draft.uuid)) ||
			    (reference.uuid.empty() && reference.name == ToStdString(draft.name))) {
				return draft.id;
			}
		}
		return {};
	}

	void EnsurePrimaryRoute()
	{
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		auto primary = std::find_if(routes.routes.begin(), routes.routes.end(),
					    [](const Route &route) { return route.primary; });
		if (primary == routes.routes.end()) {
			primary = std::find_if(routes.routes.begin(), routes.routes.end(), [&](const Route &route) {
				return OBS::Output::CanvasReferenceMatches(route.canvas, mainCanvas);
			});
		}
		if (primary == routes.routes.end()) {
			Route route;
			route.id = ToStdString(NewId());
			route.name = ToStdString(QTStr("OBSPro.Settings.Output.Main"));
			route.canvas = OBS::Output::CanvasReferenceFromCanvas(mainCanvas);
			route.primary = true;
			routes.routes.insert(routes.routes.begin(), std::move(route));
			primary = routes.routes.begin();
		}

		for (Route &route : routes.routes) {
			route.primary = &route == &*primary;
		}
		primary->name = ToStdString(QTStr("OBSPro.Settings.Output.Main"));
		primary->canvas = OBS::Output::CanvasReferenceFromCanvas(mainCanvas);
		primary->enabled = true;
		routes.schemaVersion = OBS::Output::RouteSchemaVersion;
	}

	void LoadRoutes()
	{
		routes = {};
		const char *serializedSessions = config_get_string(main->Config(), "Stream1", "PlatformSessions");
		const char *serializedRoutes = config_get_string(main->Config(), "Stream1", "OutputRoutes");
		bool loaded = false;
		if (serializedSessions && *serializedSessions) {
			OBS::Output::SessionSet sessions;
			std::string error;
			if (OBS::Output::Deserialize(serializedSessions, sessions, error)) {
				routes = OBS::Output::ToRouteSet(sessions);
				loaded = true;
			} else {
				QMessageBox::warning(destinationTabs, QTStr("OBSPro.OutputRoutes.InvalidConfiguration"),
						     QTStr("OBSPro.OutputRoutes.InvalidConfigurationText")
							     .arg(QString::fromUtf8(error.c_str())));
			}
		}
		if (!loaded && serializedRoutes && *serializedRoutes) {
			std::string error;
			if (!OBS::Output::Deserialize(serializedRoutes, routes, error)) {
				QMessageBox::warning(destinationTabs, QTStr("OBSPro.OutputRoutes.InvalidConfiguration"),
						     QTStr("OBSPro.OutputRoutes.InvalidConfigurationText")
							     .arg(QString::fromUtf8(error.c_str())));
				routes = {};
			}
		}
		EnsurePrimaryRoute();
	}

	void LoadCanvases()
	{
		canvasDrafts.clear();
		deletedCanvasUuids.clear();
		for (const OBS::Canvas &canvasRef : main->GetCanvases()) {
			obs_canvas_t *canvas = canvasRef;
			if (!canvas || (obs_canvas_get_flags(canvas) & EPHEMERAL)) {
				continue;
			}
			CanvasDraft draft;
			draft.id = QString::fromUtf8(obs_canvas_get_uuid(canvas));
			draft.uuid = draft.id;
			draft.name = QString::fromUtf8(obs_canvas_get_name(canvas));
			draft.originalName = draft.name;
			draft.existing = true;
			obs_canvas_get_video_info(canvas, &draft.info);
			canvasDrafts.emplace_back(std::move(draft));
		}
	}

	void RemoveCustomTabs(QTabWidget *tabs, const char *property)
	{
		for (int index = tabs->count() - 1; index >= 0; --index) {
			QWidget *page = tabs->widget(index);
			if (!page->property(property).isValid()) {
				continue;
			}
			tabs->removeTab(index);
			delete page;
		}
	}

	void PopulateCanvasCombo(QComboBox *combo, const CanvasReference &selected)
	{
		QSignalBlocker blocker(combo);
		combo->clear();
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		combo->addItem(QTStr("OBSPro.Settings.Canvas.Main"),
			       QString::fromUtf8(obs_canvas_get_uuid(mainCanvas)));
		for (const CanvasDraft &draft : canvasDrafts) {
			combo->addItem(draft.name, draft.id);
		}
		const QString selectedId = CanvasIdForReference(selected);
		const int index = combo->findData(selectedId);
		combo->setCurrentIndex(index >= 0 ? index : 0);
	}

	void PopulateOutputCombo(QComboBox *combo, const QString &selectedRouteId)
	{
		QSignalBlocker blocker(combo);
		combo->clear();
		for (const Route &route : routes.routes) {
			combo->addItem(QString::fromUtf8(route.name.c_str()), QString::fromUtf8(route.id.c_str()));
		}
		const int index = combo->findData(selectedRouteId);
		combo->setCurrentIndex(index >= 0 ? index : 0);
	}

	void PopulateEncoderCombo(QComboBox *combo, obs_encoder_type encoderType, const std::string &selected)
	{
		QSignalBlocker blocker(combo);
		combo->clear();
		combo->addItem(QTStr("OBSPro.Settings.Output.InheritEncoder"), QString());
		const char *id = nullptr;
		for (size_t index = 0; obs_enum_encoder_types(index, &id); ++index) {
			if (obs_get_encoder_type(id) != encoderType ||
			    (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))) {
				continue;
			}
			combo->addItem(QString::fromUtf8(obs_encoder_get_display_name(id)), QString::fromUtf8(id));
		}
		combo->model()->sort(0);
		const int selectedIndex = combo->findData(QString::fromUtf8(selected.c_str()));
		combo->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
	}

	void PopulateServiceCombo(QComboBox *combo, const std::string &selected)
	{
		QSignalBlocker blocker(combo);
		combo->clear();
		const char *id = nullptr;
		for (size_t index = 0; obs_enum_service_types(index, &id); ++index) {
			OBSProperties properties = obs_get_service_properties(id);
			if (!properties) {
				continue;
			}
			combo->addItem(QString::fromUtf8(obs_service_get_display_name(id)), QString::fromUtf8(id));
		}
		combo->model()->sort(0);
		int selectedIndex = combo->findData(QString::fromUtf8(selected.c_str()));
		if (selectedIndex < 0) {
			selectedIndex = combo->findData(QStringLiteral("rtmp_common"));
		}
		combo->setCurrentIndex(std::max(0, selectedIndex));
	}

	void SyncDestinationUi(DestinationUi &ui)
	{
		auto [route, destination] = FindDestination(ui.id);
		if (!route || !destination) {
			return;
		}
		destination->enabled = ui.enabled->isChecked();
		destination->name = ToStdString(ui.name->text().trimmed());
		destination->priority = static_cast<uint32_t>(ui.priority->value());
		destination->service = ToStdString(ui.serviceType->currentData().toString());
		if (ui.properties) {
			obs_data_t *settings = ui.properties->GetSettings();
			destination->serviceSettingsJson = SettingsJson(settings);
			destination->serviceName = obs_data_get_string(settings, "service");
			destination->server = obs_data_get_string(settings, "server");
			destination->streamKey = obs_data_get_string(settings, "key");
			destination->useAuthentication = obs_data_get_bool(settings, "use_auth");
			destination->username = obs_data_get_string(settings, "username");
			destination->password = obs_data_get_string(settings, "password");
		}
	}

	void CreateServiceProperties(DestinationUi &ui)
	{
		if (ui.properties) {
			delete ui.properties;
			ui.properties = nullptr;
		}
		auto [route, destination] = FindDestination(ui.id);
		if (!route || !destination || destination->service.empty()) {
			return;
		}
		OBSDataAutoRelease defaults = obs_service_defaults(destination->service.c_str());
		OBSDataAutoRelease settings = SettingsFromJson(destination->serviceSettingsJson, defaults);
		if (destination->serviceSettingsJson.empty()) {
			if (!destination->serviceName.empty()) {
				obs_data_set_string(settings, "service", destination->serviceName.c_str());
			}
			if (!destination->server.empty()) {
				obs_data_set_string(settings, "server", destination->server.c_str());
			}
			if (!destination->streamKey.empty()) {
				obs_data_set_string(settings, "key", destination->streamKey.c_str());
			}
			obs_data_set_bool(settings, "use_auth", destination->useAuthentication);
			obs_data_set_string(settings, "username", destination->username.c_str());
			obs_data_set_string(settings, "password", destination->password.c_str());
		}

		ui.properties = new OBSPropertiesView(settings.Get(), destination->service.c_str(),
						      (PropertiesReloadCallback)obs_get_service_properties, 170);
		ui.properties->setFrameShape(QFrame::NoFrame);
		ui.properties->setScrolling(false);
		ui.propertiesLayout->addWidget(ui.properties);
		QObject::connect(ui.properties, &OBSPropertiesView::Changed, ui.page, [this]() { MarkChanged(); });
	}

	void MoveDestination(const QString &destinationId, const QString &targetRouteId)
	{
		auto [sourceRoute, destination] = FindDestination(destinationId);
		Route *targetRoute = FindRoute(targetRouteId);
		if (!sourceRoute || !destination || !targetRoute || sourceRoute == targetRoute) {
			return;
		}
		Destination moved = std::move(*destination);
		sourceRoute->destinations.erase(
			std::remove_if(sourceRoute->destinations.begin(), sourceRoute->destinations.end(),
				       [&](const Destination &item) { return item.id == ToStdString(destinationId); }),
			sourceRoute->destinations.end());
		targetRoute->destinations.emplace_back(std::move(moved));
	}

	void BuildDestinationTabs()
	{
		RemoveCustomTabs(destinationTabs, "outputDestinationId");
		destinationUis.clear();
		for (Route &route : routes.routes) {
			for (Destination &destination : route.destinations) {
				auto ui = std::make_unique<DestinationUi>();
				ui->id = QString::fromUtf8(destination.id.c_str());
				ui->page = new QWidget(destinationTabs);
				ui->page->setProperty("outputDestinationId", ui->id);
				QWidget *contents = nullptr;
				auto *layout = CreateNativeSettingsPage(ui->page, contents);
				auto *settings = new QGroupBox(QTStr("Basic.Settings.Stream.Destination"), contents);
				auto *form = new QFormLayout(settings);
				ConfigureNativeSettingsForm(form);
				layout->addWidget(settings);

				ui->enabled = new QCheckBox(QTStr("OBSPro.OutputRoutes.Enabled"), settings);
				ui->enabled->setChecked(destination.enabled);
				AddNativeSettingsRow(form, settings, QString(), ui->enabled);
				ui->name = new QLineEdit(QString::fromUtf8(destination.name.c_str()), settings);
				AddNativeSettingsRow(form, settings, QTStr("OBSPro.OutputRoutes.Name"), ui->name);
				ui->output = new QComboBox(settings);
				PopulateOutputCombo(ui->output, QString::fromUtf8(route.id.c_str()));
				AddNativeSettingsRow(form, settings, QTStr("OBSPro.Settings.Stream.EncodedOutput"),
						     ui->output);
				ui->serviceType = new QComboBox(settings);
				PopulateServiceCombo(ui->serviceType, destination.service);
				AddNativeSettingsRow(form, settings, QTStr("Basic.Settings.Stream.Service"),
						     ui->serviceType);
				ui->priority = new QSpinBox(settings);
				ui->priority->setRange(0, 999);
				ui->priority->setValue(static_cast<int>(destination.priority));
				AddNativeSettingsRow(form, settings, QTStr("OBSPro.OutputRoutes.Priority"),
						     ui->priority);

				auto *properties =
					new QGroupBox(QTStr("OBSPro.Settings.Stream.ServiceSettings"), contents);
				ui->propertiesLayout = new QVBoxLayout(properties);
				ui->propertiesLayout->setContentsMargins(9, 2, 9, 9);
				layout->addWidget(properties);
				layout->addStretch();
				auto *remove =
					new QPushButton(QTStr("OBSPro.Settings.Stream.RemoveDestination"), contents);
				layout->addWidget(remove, 0, Qt::AlignRight);

				DestinationUi *raw = ui.get();
				QObject::connect(ui->enabled, &QCheckBox::toggled, ui->page,
						 [this]() { MarkChanged(); });
				QObject::connect(ui->name, &QLineEdit::textChanged, ui->page,
						 [this, raw](const QString &text) {
							 auto [currentRoute, current] = FindDestination(raw->id);
							 if (currentRoute && current) {
								 current->name = ToStdString(text.trimmed());
							 }
							 destinationTabs->setTabText(
								 destinationTabs->indexOf(raw->page), text.trimmed());
							 RefreshAssignedDestinationLabels();
							 MarkChanged();
						 });
				QObject::connect(ui->priority, &QSpinBox::valueChanged, ui->page,
						 [this](int) { MarkChanged(); });
				QObject::connect(
					ui->output, &QComboBox::currentIndexChanged, ui->page, [this, raw](int) {
						SyncDestinationUi(*raw);
						MoveDestination(raw->id, raw->output->currentData().toString());
						RefreshAssignedDestinationLabels();
						MarkChanged();
					});
				QObject::connect(ui->serviceType, &QComboBox::currentIndexChanged, ui->page,
						 [this, raw](int) {
							 auto [currentRoute, current] = FindDestination(raw->id);
							 if (!currentRoute || !current) {
								 return;
							 }
							 current->service = ToStdString(
								 raw->serviceType->currentData().toString());
							 current->serviceSettingsJson.clear();
							 current->server.clear();
							 current->streamKey.clear();
							 CreateServiceProperties(*raw);
							 MarkChanged();
						 });
				QObject::connect(remove, &QPushButton::clicked, ui->page,
						 [this, id = ui->id]() { RemoveDestination(id); });

				destinationTabs->addTab(ui->page, QString::fromUtf8(destination.name.c_str()));
				destinationUis.emplace_back(std::move(ui));
				CreateServiceProperties(*destinationUis.back());
			}
		}
	}

	void AddDestination()
	{
		SyncAll();
		Route *primary = PrimaryRoute();
		if (!primary) {
			return;
		}
		Destination destination;
		destination.id = ToStdString(NewId());
		const QString baseName = QTStr("OBSPro.Settings.Stream.NewDestination");
		QString name = baseName;
		int suffix = 2;
		auto nameExists = [&](const QString &candidate) {
			return std::any_of(routes.routes.begin(), routes.routes.end(), [&](const Route &route) {
				return std::any_of(route.destinations.begin(), route.destinations.end(),
						   [&](const Destination &item) {
							   return item.name == ToStdString(candidate);
						   });
			});
		};
		while (nameExists(name)) {
			name = QStringLiteral("%1 %2").arg(baseName).arg(suffix++);
		}
		destination.name = ToStdString(name);
		destination.service = "rtmp_common";
		destination.priority = static_cast<uint32_t>(destinationUis.size());
		primary->destinations.emplace_back(std::move(destination));
		BuildDestinationTabs();
		destinationTabs->setCurrentIndex(destinationTabs->count() - 1);
		RefreshAssignedDestinationLabels();
		MarkChanged();
	}

	void RemoveDestination(const QString &id)
	{
		SyncAll();
		for (Route &route : routes.routes) {
			route.destinations.erase(std::remove_if(route.destinations.begin(), route.destinations.end(),
								[&](const Destination &destination) {
									return destination.id == ToStdString(id);
								}),
						 route.destinations.end());
		}
		BuildDestinationTabs();
		RefreshAssignedDestinationLabels();
		MarkChanged();
	}

	void SyncRouteUi(RouteUi &ui)
	{
		Route *route = FindRoute(ui.id);
		if (!route) {
			return;
		}
		route->enabled = ui.enabled->isChecked();
		route->name = ToStdString(ui.name->text().trimmed());
		route->canvas = CanvasReferenceForId(ui.canvas->currentData().toString());
		route->videoEncoderId = ToStdString(ui.videoEncoder->currentData().toString());
		route->audioEncoderId = ToStdString(ui.audioEncoder->currentData().toString());
		route->audioMix = static_cast<uint32_t>(ui.audioMix->value() - 1);
		route->failoverMode = static_cast<FailoverMode>(ui.failoverMode->currentData().toInt());
		if (ui.videoProperties) {
			route->videoEncoderSettingsJson = SettingsJson(ui.videoProperties->GetSettings());
		}
		if (ui.audioProperties) {
			route->audioEncoderSettingsJson = SettingsJson(ui.audioProperties->GetSettings());
		}
	}

	void CreateEncoderProperties(RouteUi &ui, bool video)
	{
		Route *route = FindRoute(ui.id);
		if (!route) {
			return;
		}
		OBSPropertiesView *&view = video ? ui.videoProperties : ui.audioProperties;
		QLabel *&notice = video ? ui.videoInheritanceNotice : ui.audioInheritanceNotice;
		QVBoxLayout *layout = video ? ui.videoPropertiesLayout : ui.audioPropertiesLayout;
		if (view) {
			delete view;
			view = nullptr;
		}
		if (notice) {
			delete notice;
			notice = nullptr;
		}
		const std::string &encoderId = video ? route->videoEncoderId : route->audioEncoderId;
		const std::string &serialized = video ? route->videoEncoderSettingsJson
						      : route->audioEncoderSettingsJson;
		if (encoderId.empty()) {
			notice = new QLabel(QTStr("OBSPro.Settings.Output.InheritEncoderDescription"), ui.page);
			notice->setWordWrap(true);
			layout->addWidget(notice);
			return;
		}
		OBSDataAutoRelease defaults = obs_encoder_defaults(encoderId.c_str());
		OBSDataAutoRelease settings = SettingsFromJson(serialized, defaults);
		view = new OBSPropertiesView(settings.Get(), encoderId.c_str(),
					     (PropertiesReloadCallback)obs_get_encoder_properties, 170);
		view->setFrameShape(QFrame::NoFrame);
		view->setScrolling(false);
		layout->addWidget(view);
		QObject::connect(view, &OBSPropertiesView::Changed, ui.page, [this]() { MarkChanged(); });
	}

	void RefreshDestinationOutputCombos()
	{
		for (auto &ui : destinationUis) {
			auto [route, destination] = FindDestination(ui->id);
			if (route && destination) {
				PopulateOutputCombo(ui->output, QString::fromUtf8(route->id.c_str()));
			}
		}
	}

	void RefreshAssignedDestinationLabels()
	{
		for (auto &ui : routeUis) {
			Route *route = FindRoute(ui->id);
			if (!route || !ui->assignedDestinations) {
				continue;
			}
			QStringList names;
			for (const Destination &destination : route->destinations) {
				names << QString::fromUtf8(destination.name.c_str());
			}
			ui->assignedDestinations->setText(
				QTStr("OBSPro.Settings.Output.AssignedDestinations") + QStringLiteral(": ") +
				(names.isEmpty() ? QTStr("None") : names.join(QStringLiteral(", "))));
		}
	}

	void BuildOutputTabs()
	{
		RemoveCustomTabs(outputTabs, "outputRouteId");
		routeUis.clear();
		if (outputTabs->count() > 0) {
			outputTabs->setTabText(0, QTStr("OBSPro.Settings.Output.Main"));
		}
		int insertIndex = 1;
		for (Route &route : routes.routes) {
			if (route.primary) {
				continue;
			}
			auto ui = std::make_unique<RouteUi>();
			ui->id = QString::fromUtf8(route.id.c_str());
			ui->page = new QWidget(outputTabs);
			ui->page->setProperty("outputRouteId", ui->id);
			QWidget *contents = nullptr;
			auto *contentsLayout = CreateNativeSettingsPage(ui->page, contents);
			auto *settings = new QGroupBox(QTStr("Basic.Settings.Output.Adv.Streaming.Settings"), contents);
			auto *form = new QFormLayout(settings);
			ConfigureNativeSettingsForm(form);
			contentsLayout->addWidget(settings);

			ui->enabled = new QCheckBox(QTStr("OBSPro.OutputRoutes.Enabled"), settings);
			ui->enabled->setChecked(route.enabled);
			AddNativeSettingsRow(form, settings, QString(), ui->enabled);
			ui->name = new QLineEdit(QString::fromUtf8(route.name.c_str()), settings);
			AddNativeSettingsRow(form, settings, QTStr("OBSPro.OutputRoutes.Name"), ui->name);
			ui->canvas = new QComboBox(settings);
			PopulateCanvasCombo(ui->canvas, route.canvas);
			AddNativeSettingsRow(form, settings, QTStr("OBSPro.OutputRoutes.Canvas"), ui->canvas);
			ui->videoEncoder = new QComboBox(settings);
			PopulateEncoderCombo(ui->videoEncoder, OBS_ENCODER_VIDEO, route.videoEncoderId);
			AddNativeSettingsRow(form, settings, QTStr("Basic.Settings.Output.Encoder.Video"),
					     ui->videoEncoder);
			ui->audioEncoder = new QComboBox(settings);
			PopulateEncoderCombo(ui->audioEncoder, OBS_ENCODER_AUDIO, route.audioEncoderId);
			AddNativeSettingsRow(form, settings, QTStr("Basic.Settings.Output.Encoder.Audio"),
					     ui->audioEncoder);
			ui->audioMix = new QSpinBox(settings);
			ui->audioMix->setRange(1, MAX_AUDIO_MIXES);
			ui->audioMix->setValue(static_cast<int>(route.audioMix + 1));
			AddNativeSettingsRow(form, settings, QTStr("OBSPro.Settings.Output.AudioMix"), ui->audioMix);
			ui->failoverMode = new QComboBox(settings);
			ui->failoverMode->addItem(QTStr("OBSPro.Settings.Output.Parallel"),
						  static_cast<int>(FailoverMode::ClientParallel));
			ui->failoverMode->addItem(QTStr("OBSPro.Settings.Output.Sequential"),
						  static_cast<int>(FailoverMode::ClientSequential));
			ui->failoverMode->setCurrentIndex(
				std::max(0, ui->failoverMode->findData(static_cast<int>(route.failoverMode))));
			AddNativeSettingsRow(form, settings, QTStr("OBSPro.Settings.Output.DeliveryMode"),
					     ui->failoverMode);

			auto *videoGroup =
				new QGroupBox(QTStr("OBSPro.Settings.Output.VideoEncoderSettings"), contents);
			ui->videoPropertiesLayout = new QVBoxLayout(videoGroup);
			ui->videoPropertiesLayout->setContentsMargins(8, 2, 8, 8);
			contentsLayout->addWidget(videoGroup);
			auto *audioGroup =
				new QGroupBox(QTStr("OBSPro.Settings.Output.AudioEncoderSettings"), contents);
			ui->audioPropertiesLayout = new QVBoxLayout(audioGroup);
			ui->audioPropertiesLayout->setContentsMargins(8, 2, 8, 8);
			contentsLayout->addWidget(audioGroup);

			ui->assignedDestinations = new QLabel(contents);
			ui->assignedDestinations->setWordWrap(true);
			contentsLayout->addWidget(ui->assignedDestinations);
			contentsLayout->addStretch();
			auto *remove = new QPushButton(QTStr("OBSPro.Settings.Output.Remove"), contents);
			contentsLayout->addWidget(remove, 0, Qt::AlignRight);

			RouteUi *raw = ui.get();
			QObject::connect(ui->enabled, &QCheckBox::toggled, ui->page, [this]() { MarkChanged(); });
			QObject::connect(ui->name, &QLineEdit::textChanged, ui->page, [this, raw](const QString &text) {
				if (Route *route = FindRoute(raw->id)) {
					route->name = ToStdString(text.trimmed());
				}
				outputTabs->setTabText(outputTabs->indexOf(raw->page), text.trimmed());
				RefreshDestinationOutputCombos();
				MarkChanged();
			});
			QObject::connect(ui->canvas, &QComboBox::currentIndexChanged, ui->page, [this, raw](int) {
				if (Route *route = FindRoute(raw->id)) {
					route->canvas = CanvasReferenceForId(raw->canvas->currentData().toString());
				}
				MarkChanged();
			});
			QObject::connect(ui->audioMix, &QSpinBox::valueChanged, ui->page,
					 [this](int) { MarkChanged(); });
			QObject::connect(ui->failoverMode, &QComboBox::currentIndexChanged, ui->page,
					 [this](int) { MarkChanged(); });
			QObject::connect(ui->videoEncoder, &QComboBox::currentIndexChanged, ui->page, [this, raw](int) {
				Route *current = FindRoute(raw->id);
				if (!current) {
					return;
				}
				current->videoEncoderId = ToStdString(raw->videoEncoder->currentData().toString());
				current->videoEncoderSettingsJson.clear();
				CreateEncoderProperties(*raw, true);
				MarkChanged();
			});
			QObject::connect(ui->audioEncoder, &QComboBox::currentIndexChanged, ui->page, [this, raw](int) {
				Route *current = FindRoute(raw->id);
				if (!current) {
					return;
				}
				current->audioEncoderId = ToStdString(raw->audioEncoder->currentData().toString());
				current->audioEncoderSettingsJson.clear();
				CreateEncoderProperties(*raw, false);
				MarkChanged();
			});
			QObject::connect(remove, &QPushButton::clicked, ui->page,
					 [this, id = ui->id]() { RemoveOutput(id); });

			outputTabs->insertTab(insertIndex++, ui->page, QString::fromUtf8(route.name.c_str()));
			routeUis.emplace_back(std::move(ui));
			CreateEncoderProperties(*routeUis.back(), true);
			CreateEncoderProperties(*routeUis.back(), false);
		}
		RefreshDestinationOutputCombos();
		RefreshAssignedDestinationLabels();
	}

	void AddOutput()
	{
		SyncAll();
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		Route route;
		route.id = ToStdString(NewId());
		const QString baseName = QTStr("OBSPro.Settings.Output.New");
		QString name = baseName;
		int suffix = 2;
		auto nameExists = [&](const QString &candidate) {
			return std::any_of(routes.routes.begin(), routes.routes.end(),
					   [&](const Route &item) { return item.name == ToStdString(candidate); });
		};
		while (nameExists(name)) {
			name = QStringLiteral("%1 %2").arg(baseName).arg(suffix++);
		}
		route.name = ToStdString(name);
		route.canvas = OBS::Output::CanvasReferenceFromCanvas(mainCanvas);
		route.failoverMode = FailoverMode::ClientParallel;
		routes.routes.emplace_back(std::move(route));
		BuildOutputTabs();
		for (int index = 0; index < outputTabs->count(); ++index) {
			if (outputTabs->widget(index)->property("outputRouteId").toString() ==
			    QString::fromUtf8(routes.routes.back().id.c_str())) {
				outputTabs->setCurrentIndex(index);
				break;
			}
		}
		MarkChanged();
	}

	void RemoveOutput(const QString &id)
	{
		SyncAll();
		Route *route = FindRoute(id);
		if (!route || route->primary) {
			return;
		}
		if (!route->destinations.empty()) {
			QMessageBox::information(outputTabs, QTStr("OBSPro.Settings.Output.InUse"),
						 QTStr("OBSPro.Settings.Output.MoveDestinationsFirst"));
			return;
		}
		routes.routes.erase(std::remove_if(routes.routes.begin(), routes.routes.end(),
						   [&](const Route &item) { return item.id == ToStdString(id); }),
				    routes.routes.end());
		BuildOutputTabs();
		MarkChanged();
	}

	void SyncCanvasUi(CanvasUi &ui)
	{
		CanvasDraft *draft = FindCanvasDraft(ui.id);
		if (!draft) {
			return;
		}
		draft->name = ui.name->text().trimmed();
		uint32_t width = 0;
		uint32_t height = 0;
		if (ParseResolution(ui.baseResolution->currentText(), width, height)) {
			draft->info.base_width = width;
			draft->info.base_height = height;
		}
		if (ParseResolution(ui.outputResolution->currentText(), width, height)) {
			draft->info.output_width = width;
			draft->info.output_height = height;
		}
		switch (ui.fpsType->currentIndex()) {
		case 0:
			ParseCommonFps(ui.fpsCommon->currentText(), draft->info.fps_num, draft->info.fps_den);
			break;
		case 1:
			draft->info.fps_num = static_cast<uint32_t>(ui.fpsInteger->value());
			draft->info.fps_den = 1;
			break;
		default:
			draft->info.fps_num = static_cast<uint32_t>(ui.fpsNumerator->value());
			draft->info.fps_den = static_cast<uint32_t>(ui.fpsDenominator->value());
			break;
		}
		draft->info.scale_type = static_cast<obs_scale_type>(ui.downscaleFilter->currentData().toInt());
	}

	QString CanvasModeText(CanvasCreationMode mode) const
	{
		switch (mode) {
		case CanvasCreationMode::Blank:
			return QTStr("OBSPro.Settings.Canvas.ModeBlank");
		case CanvasCreationMode::IndependentSources:
			return QTStr("OBSPro.Settings.Canvas.ModeIndependent");
		case CanvasCreationMode::ReuseSources:
		default:
			return QTStr("OBSPro.Settings.Canvas.ModeReuse");
		}
	}

	void RefreshRouteCanvasCombos()
	{
		for (auto &ui : routeUis) {
			Route *route = FindRoute(ui->id);
			if (route) {
				PopulateCanvasCombo(ui->canvas, route->canvas);
			}
		}
	}

	void BuildCanvasTabs()
	{
		RemoveCustomTabs(canvasTabs, "canvasDraftId");
		canvasUis.clear();
		for (CanvasDraft &draft : canvasDrafts) {
			auto ui = std::make_unique<CanvasUi>();
			ui->id = draft.id;
			ui->page = new QWidget(canvasTabs);
			ui->page->setProperty("canvasDraftId", draft.id);
			QWidget *contents = nullptr;
			auto *layout = CreateNativeSettingsPage(ui->page, contents, false);
			auto *general = new QGroupBox(QTStr("Basic.Settings.General"), contents);
			general->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
			auto *form = new QFormLayout(general);
			ConfigureNativeSettingsForm(form);
			layout->addWidget(general);
			ui->name = new QLineEdit(draft.name, general);
			AddNativeSettingsRow(form, general, QTStr("OBSPro.OutputRoutes.Name"), ui->name);

			auto *baseResolutionLayout = new QHBoxLayout;
			baseResolutionLayout->setContentsMargins(0, 0, 0, 0);
			baseResolutionLayout->setSpacing(6);
			ui->baseResolution = new QComboBox(general);
			ui->baseAspect = new QLabel(general);
			baseResolutionLayout->addWidget(ui->baseResolution);
			baseResolutionLayout->addWidget(ui->baseAspect);
			PopulateCanvasResolutionCombo(ui->baseResolution, draft.info, true);
			UpdateAspectRatio(ui->baseResolution, ui->baseAspect);

			auto *outputResolutionLayout = new QHBoxLayout;
			outputResolutionLayout->setContentsMargins(0, 0, 0, 0);
			outputResolutionLayout->setSpacing(6);
			ui->outputResolution = new QComboBox(general);
			ui->outputAspect = new QLabel(general);
			outputResolutionLayout->addWidget(ui->outputResolution);
			outputResolutionLayout->addWidget(ui->outputAspect);
			PopulateCanvasResolutionCombo(ui->outputResolution, draft.info, false);
			UpdateAspectRatio(ui->outputResolution, ui->outputAspect);

			auto *validator = new QRegularExpressionValidator(
				QRegularExpression(QStringLiteral("\\d{2,5}[xX]\\d{2,5}")), general);
			ui->baseResolution->lineEdit()->setValidator(validator);
			ui->outputResolution->lineEdit()->setValidator(validator);
			form->addRow(CreateNativeSettingsLabel(general, QTStr("Basic.Settings.Video.BaseResolution"),
							       ui->baseResolution),
				     baseResolutionLayout);
			form->addRow(CreateNativeSettingsLabel(general, QTStr("Basic.Settings.Video.ScaledResolution"),
							       ui->outputResolution),
				     outputResolutionLayout);
			ui->downscaleFilter = new QComboBox(general);
			PopulateCanvasDownscaleFilter(ui->downscaleFilter, draft.info,
						      ui->baseResolution->currentText(),
						      ui->outputResolution->currentText());
			AddNativeSettingsRow(form, general, QTStr("Basic.Settings.Video.DownscaleFilter"),
					     ui->downscaleFilter);

			ui->fpsType = new QComboBox(general);
			ui->fpsType->addItem(QTStr("Basic.Settings.Video.FPSCommon"));
			ui->fpsType->addItem(QTStr("Basic.Settings.Video.FPSInteger"));
			ui->fpsType->addItem(QTStr("Basic.Settings.Video.FPSFraction"));
			ui->fpsTypes = new QStackedWidget(general);
			ui->fpsTypes->setFrameShape(QFrame::NoFrame);
			ui->fpsTypes->setLineWidth(0);

			auto *commonPage = new QWidget(ui->fpsTypes);
			auto *commonLayout = new QHBoxLayout(commonPage);
			commonLayout->setContentsMargins(0, 0, 0, 0);
			ui->fpsCommon = new QComboBox(commonPage);
			for (const QString &value :
			     {QStringLiteral("10"), QStringLiteral("20"), QStringLiteral("24 NTSC"),
			      QStringLiteral("25 PAL"), QStringLiteral("29.97"), QStringLiteral("30"),
			      QStringLiteral("48"), QStringLiteral("50 PAL"), QStringLiteral("59.94"),
			      QStringLiteral("60")}) {
				ui->fpsCommon->addItem(value);
			}
			commonLayout->addWidget(ui->fpsCommon, 0, Qt::AlignTop);
			ui->fpsTypes->addWidget(commonPage);

			auto *integerPage = new QWidget(ui->fpsTypes);
			auto *integerLayout = new QHBoxLayout(integerPage);
			integerLayout->setContentsMargins(0, 0, 0, 0);
			ui->fpsInteger = new QSpinBox(integerPage);
			ui->fpsInteger->setRange(1, 120);
			integerLayout->addWidget(ui->fpsInteger, 0, Qt::AlignTop);
			ui->fpsTypes->addWidget(integerPage);

			auto *fractionPage = new QWidget(ui->fpsTypes);
			auto *fractionLayout = new QFormLayout(fractionPage);
			fractionLayout->setContentsMargins(0, 0, 0, 0);
			ui->fpsNumerator = new QSpinBox(fractionPage);
			ui->fpsNumerator->setRange(1, 1000000);
			ui->fpsDenominator = new QSpinBox(fractionPage);
			ui->fpsDenominator->setRange(1, 1000000);
			fractionLayout->addRow(QTStr("Basic.Settings.Video.Numerator"), ui->fpsNumerator);
			fractionLayout->addRow(QTStr("Basic.Settings.Video.Denominator"), ui->fpsDenominator);
			ui->fpsTypes->addWidget(fractionPage);

			int commonFpsIndex = -1;
			for (int index = 0; index < ui->fpsCommon->count(); ++index) {
				uint32_t numerator = 0;
				uint32_t denominator = 0;
				if (ParseCommonFps(ui->fpsCommon->itemText(index), numerator, denominator) &&
				    numerator == draft.info.fps_num && denominator == draft.info.fps_den) {
					commonFpsIndex = index;
					break;
				}
			}
			if (commonFpsIndex >= 0) {
				ui->fpsType->setCurrentIndex(0);
				ui->fpsCommon->setCurrentIndex(commonFpsIndex);
			} else if (draft.info.fps_den == 1) {
				ui->fpsType->setCurrentIndex(1);
				ui->fpsInteger->setValue(static_cast<int>(draft.info.fps_num));
			} else {
				ui->fpsType->setCurrentIndex(2);
				ui->fpsNumerator->setValue(static_cast<int>(draft.info.fps_num));
				ui->fpsDenominator->setValue(static_cast<int>(draft.info.fps_den));
			}
			ui->fpsTypes->setCurrentIndex(ui->fpsType->currentIndex());
			form->addRow(ui->fpsType, ui->fpsTypes);
			if (!draft.existing) {
				auto *mode = new QLabel(CanvasModeText(draft.creationMode), general);
				mode->setWordWrap(true);
				AddNativeSettingsRow(form, general, QTStr("OBSPro.Settings.Canvas.CreationMode"), mode);
			}
			auto *description = new QLabel(QTStr("OBSPro.Settings.Canvas.SceneSetDescription"), contents);
			description->setWordWrap(true);
			layout->addWidget(description);
			layout->addStretch();
			auto *remove = new QPushButton(QTStr("OBSPro.Settings.Canvas.Remove"), contents);
			layout->addWidget(remove, 0, Qt::AlignRight);

			CanvasUi *raw = ui.get();
			QObject::connect(ui->name, &QLineEdit::textChanged, ui->page, [this, raw](const QString &text) {
				if (CanvasDraft *draft = FindCanvasDraft(raw->id)) {
					draft->name = text.trimmed();
				}
				canvasTabs->setTabText(canvasTabs->indexOf(raw->page), text.trimmed());
				RefreshRouteCanvasCombos();
				MarkChanged();
			});
			QObject::connect(ui->baseResolution, &QComboBox::currentTextChanged, ui->page,
					 [this, raw](const QString &) {
						 UpdateAspectRatio(raw->baseResolution, raw->baseAspect);
						 if (CanvasDraft *draft = FindCanvasDraft(raw->id)) {
							 PopulateCanvasDownscaleFilter(
								 raw->downscaleFilter, draft->info,
								 raw->baseResolution->currentText(),
								 raw->outputResolution->currentText());
						 }
						 MarkChanged();
					 });
			QObject::connect(ui->outputResolution, &QComboBox::currentTextChanged, ui->page,
					 [this, raw](const QString &) {
						 UpdateAspectRatio(raw->outputResolution, raw->outputAspect);
						 if (CanvasDraft *draft = FindCanvasDraft(raw->id)) {
							 PopulateCanvasDownscaleFilter(
								 raw->downscaleFilter, draft->info,
								 raw->baseResolution->currentText(),
								 raw->outputResolution->currentText());
						 }
						 MarkChanged();
					 });
			QObject::connect(ui->fpsType, &QComboBox::currentIndexChanged, ui->page,
					 [this, raw](int index) {
						 raw->fpsTypes->setCurrentIndex(index);
						 MarkChanged();
					 });
			QObject::connect(ui->fpsCommon, &QComboBox::currentTextChanged, ui->page,
					 [this](const QString &) { MarkChanged(); });
			QObject::connect(ui->fpsInteger, &QSpinBox::valueChanged, ui->page,
					 [this](int) { MarkChanged(); });
			QObject::connect(ui->fpsNumerator, &QSpinBox::valueChanged, ui->page,
					 [this](int) { MarkChanged(); });
			QObject::connect(ui->fpsDenominator, &QSpinBox::valueChanged, ui->page,
					 [this](int) { MarkChanged(); });
			QObject::connect(ui->downscaleFilter, &QComboBox::currentIndexChanged, ui->page,
					 [this](int) { MarkChanged(); });
			QObject::connect(remove, &QPushButton::clicked, ui->page,
					 [this, id = ui->id]() { RemoveCanvas(id); });

			canvasTabs->addTab(ui->page, draft.name);
			canvasUis.emplace_back(std::move(ui));
		}
		RefreshRouteCanvasCombos();
	}

	QString UniqueCanvasName() const
	{
		QString base = QTStr("OBSPro.OutputRoutes.NewCanvas");
		QString candidate = base;
		int suffix = 2;
		auto exists = [&](const QString &name) {
			OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
			if (mainCanvas && name == QString::fromUtf8(obs_canvas_get_name(mainCanvas))) {
				return true;
			}
			for (const QString &uuid : deletedCanvasUuids) {
				OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(ToStdString(uuid).c_str());
				if (canvas && name == QString::fromUtf8(obs_canvas_get_name(canvas))) {
					return true;
				}
			}
			return std::any_of(canvasDrafts.begin(), canvasDrafts.end(),
					   [&](const CanvasDraft &draft) { return draft.name == name; });
		};
		while (exists(candidate)) {
			candidate = QStringLiteral("%1 %2").arg(base).arg(suffix++);
		}
		return candidate;
	}

	void AddCanvas(CanvasCreationMode mode)
	{
		SyncAll();
		obs_video_info info{};
		if (!obs_get_video_info(&info)) {
			return;
		}
		if (info.base_width >= info.base_height) {
			std::swap(info.base_width, info.base_height);
			std::swap(info.output_width, info.output_height);
		}
		CanvasDraft draft;
		draft.id = NewId();
		draft.uuid = QStringLiteral("pending:") + draft.id;
		draft.name = UniqueCanvasName();
		draft.info = info;
		draft.creationMode = mode;
		canvasDrafts.emplace_back(std::move(draft));
		BuildCanvasTabs();
		canvasTabs->setCurrentIndex(canvasTabs->count() - 1);
		MarkChanged();
	}

	void RemoveCanvas(const QString &id)
	{
		SyncAll();
		CanvasDraft *draft = FindCanvasDraft(id);
		if (!draft) {
			return;
		}
		const bool referenced =
			std::any_of(routes.routes.begin(), routes.routes.end(), [&](const Route &route) {
				return !route.primary &&
				       ((!route.canvas.uuid.empty() && route.canvas.uuid == ToStdString(draft->uuid)) ||
					(route.canvas.uuid.empty() && route.canvas.name == ToStdString(draft->name)));
			});
		if (referenced) {
			QMessageBox::information(canvasTabs, QTStr("OBSPro.OutputRoutes.CanvasInUse"),
						 QTStr("OBSPro.Settings.Canvas.MoveOutputsFirst"));
			return;
		}
		if (draft->existing &&
		    QMessageBox::question(canvasTabs, QTStr("OBSPro.OutputRoutes.RemoveCanvas"),
					  QTStr("OBSPro.OutputRoutes.RemoveCanvasConfirm")) != QMessageBox::Yes) {
			return;
		}
		if (draft->existing) {
			deletedCanvasUuids.insert(draft->uuid);
		}
		canvasDrafts.erase(std::remove_if(canvasDrafts.begin(), canvasDrafts.end(),
						  [&](const CanvasDraft &item) { return item.id == id; }),
				   canvasDrafts.end());
		BuildCanvasTabs();
		MarkChanged();
	}

	void SyncAll()
	{
		for (auto &ui : destinationUis) {
			SyncDestinationUi(*ui);
		}
		for (auto &ui : routeUis) {
			SyncRouteUi(*ui);
		}
		for (auto &ui : canvasUis) {
			SyncCanvasUi(*ui);
		}
	}

	bool Validate(QString &error)
	{
		SyncAll();
		for (const auto &ui : canvasUis) {
			uint32_t width = 0;
			uint32_t height = 0;
			if (!ParseResolution(ui->baseResolution->currentText(), width, height) ||
			    !ParseResolution(ui->outputResolution->currentText(), width, height)) {
				error = QTStr("OBSPro.Settings.Canvas.InvalidVideo").arg(ui->name->text().trimmed());
				return false;
			}
		}
		std::unordered_set<std::string> routeNames;
		std::unordered_set<std::string> destinationNames;
		for (const Route &route : routes.routes) {
			if (route.name.empty()) {
				error = QTStr("OBSPro.Settings.Output.NameRequired");
				return false;
			}
			if (!routeNames.insert(route.name).second) {
				error = QTStr("OBSPro.Settings.Output.DuplicateName")
						.arg(QString::fromUtf8(route.name.c_str()));
				return false;
			}
			for (const Destination &destination : route.destinations) {
				if (destination.name.empty()) {
					error = QTStr("OBSPro.Settings.Stream.NameRequired");
					return false;
				}
				if (destination.server.empty()) {
					error = QTStr("OBSPro.OutputRoutes.NameServerRequired");
					return false;
				}
				if (!destinationNames.insert(destination.name).second) {
					error = QTStr("OBSPro.Settings.Stream.DuplicateName")
							.arg(QString::fromUtf8(destination.name.c_str()));
					return false;
				}
			}
		}

		std::unordered_set<std::string> canvasNames;
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		if (mainCanvas) {
			canvasNames.emplace(obs_canvas_get_name(mainCanvas));
		}
		for (const QString &uuid : deletedCanvasUuids) {
			OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(ToStdString(uuid).c_str());
			if (canvas) {
				canvasNames.emplace(obs_canvas_get_name(canvas));
			}
		}
		for (const CanvasDraft &draft : canvasDrafts) {
			if (draft.name.isEmpty()) {
				error = QTStr("OBSPro.OutputRoutes.CanvasNameRequired");
				return false;
			}
			if (!canvasNames.insert(ToStdString(draft.name)).second) {
				error = QTStr("OBSPro.OutputRoutes.DuplicateCanvasName");
				return false;
			}
			if (draft.info.base_width < 32 || draft.info.base_height < 32 || draft.info.output_width < 32 ||
			    draft.info.output_height < 32 || draft.info.fps_num == 0 || draft.info.fps_den == 0) {
				error = QTStr("OBSPro.Settings.Canvas.InvalidVideo").arg(draft.name);
				return false;
			}
		}

		const std::vector<std::string> errors = OBS::Output::Validate(routes);
		if (!errors.empty()) {
			error = QString::fromUtf8(errors.front().c_str());
			return false;
		}
		error.clear();
		return true;
	}

	bool CanvasesChanged() const
	{
		if (!deletedCanvasUuids.empty()) {
			return true;
		}
		for (const CanvasDraft &draft : canvasDrafts) {
			if (!draft.existing || draft.name != draft.originalName) {
				return true;
			}
			OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(ToStdString(draft.uuid).c_str());
			obs_video_info current{};
			if (!canvas || !obs_canvas_get_video_info(canvas, &current) ||
			    current.base_width != draft.info.base_width ||
			    current.base_height != draft.info.base_height ||
			    current.output_width != draft.info.output_width ||
			    current.output_height != draft.info.output_height ||
			    current.fps_num != draft.info.fps_num || current.fps_den != draft.info.fps_den ||
			    current.scale_type != draft.info.scale_type) {
				return true;
			}
		}
		return false;
	}

	void ReplaceCanvasReference(const QString &oldUuid, obs_canvas_t *canvas)
	{
		const CanvasReference updated = OBS::Output::CanvasReferenceFromCanvas(canvas);
		for (Route &route : routes.routes) {
			if (route.canvas.uuid == ToStdString(oldUuid)) {
				route.canvas = updated;
			}
		}
	}

	bool ApplyCanvases(QString &error)
	{
		if (CanvasesChanged() && OutputsAreActive()) {
			error = QTStr("OBSPro.OutputRoutes.StopOutputsFirst");
			return false;
		}

		for (CanvasDraft &draft : canvasDrafts) {
			if (draft.existing) {
				OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(ToStdString(draft.uuid).c_str());
				if (!canvas) {
					error = QTStr("OBSPro.Settings.Canvas.Missing").arg(draft.name);
					return false;
				}
				obs_video_info current{};
				obs_canvas_get_video_info(canvas, &current);
				if (draft.name != draft.originalName) {
					obs_canvas_set_name(canvas, ToStdString(draft.name).c_str());
				}
				const bool videoChanged = current.base_width != draft.info.base_width ||
							  current.base_height != draft.info.base_height ||
							  current.output_width != draft.info.output_width ||
							  current.output_height != draft.info.output_height ||
							  current.fps_num != draft.info.fps_num ||
							  current.fps_den != draft.info.fps_den ||
							  current.scale_type != draft.info.scale_type;
				if (videoChanged && !obs_canvas_reset_video(canvas, &draft.info)) {
					error = QTStr("OBSPro.OutputRoutes.CanvasResetFailed");
					return false;
				}
				for (Route &route : routes.routes) {
					if (route.canvas.uuid == ToStdString(draft.uuid)) {
						route.canvas.name = ToStdString(draft.name);
					}
				}
				draft.originalName = draft.name;
				continue;
			}

			const QString pendingUuid = draft.uuid;
			const OBS::Canvas &created =
				main->AddCanvas(ToStdString(draft.name), &draft.info, ACTIVATE | SCENE_REF);
			obs_canvas_t *canvas = created;
			if (!canvas) {
				error = QTStr("OBSPro.Settings.Canvas.CreateFailed").arg(draft.name);
				return false;
			}
			const bool duplicateLayout = draft.creationMode != CanvasCreationMode::Blank;
			const bool independentSources = draft.creationMode == CanvasCreationMode::IndependentSources;
			main->InitializeCanvasSceneSets(canvas, duplicateLayout, independentSources);
			ReplaceCanvasReference(pendingUuid, canvas);
			draft.uuid = QString::fromUtf8(obs_canvas_get_uuid(canvas));
			draft.id = draft.uuid;
			draft.existing = true;
			draft.originalName = draft.name;
		}

		for (const QString &uuid : deletedCanvasUuids) {
			OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(ToStdString(uuid).c_str());
			if (canvas) {
				main->RemoveCanvas(OBSCanvas(canvas));
			}
		}
		deletedCanvasUuids.clear();
		main->SaveProject();
		main->RefreshCanvasTabs();
		return true;
	}

	bool Save(QString &error)
	{
		if (!Validate(error) || !ApplyCanvases(error)) {
			return false;
		}
		const std::string serialized = OBS::Output::Serialize(routes);
		config_set_string(main->Config(), "Stream1", "OutputRoutes", serialized.c_str());
		const std::string serializedSessions = OBS::Output::Serialize(OBS::Output::MigrateRouteSet(routes));
		config_set_string(main->Config(), "Stream1", "PlatformSessions", serializedSessions.c_str());
		Load();
		return true;
	}

	void Load()
	{
		loading = true;
		LoadRoutes();
		LoadCanvases();
		BuildDestinationTabs();
		BuildOutputTabs();
		BuildCanvasTabs();
		loading = false;
	}
};

OBSOutputRoutesSettings::OBSOutputRoutesSettings(OBSBasic *main, QWidget *streamPage, QWidget *outputPage,
						 QWidget *videoPage, std::function<void()> changedCallback)
	: impl(std::make_unique<Impl>(main, streamPage, outputPage, videoPage, std::move(changedCallback)))
{
}

OBSOutputRoutesSettings::~OBSOutputRoutesSettings() = default;

void OBSOutputRoutesSettings::Load()
{
	impl->Load();
}

bool OBSOutputRoutesSettings::Validate(QString &error)
{
	return impl->Validate(error);
}

bool OBSOutputRoutesSettings::Save(QString &error)
{
	return impl->Save(error);
}
