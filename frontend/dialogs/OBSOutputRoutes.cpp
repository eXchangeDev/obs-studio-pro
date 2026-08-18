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

#include <settings/OBSNativeSettingsPage.hpp>
#include <utility/PlatformSession.hpp>
#include <utility/OutputRoute.hpp>
#include <widgets/OBSBasic.hpp>

#include <obs-frontend-api.h>
#include <properties-view.hpp>
#include <qt-wrappers.hpp>
#include <util/config-file.h>

#include <QAbstractItemModel>
#include <QBoxLayout>
#include <QButtonGroup>
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
#include <QRadioButton>
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
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {

using OBS::Output::CanvasReference;
using OBS::Output::Destination;
using OBS::Output::FailoverMode;
using OBS::Output::Kind;
using OBS::Output::Route;
using OBS::Output::RouteSet;

constexpr char PrimaryDestinationId[] = "primary-destination";

std::string ToStdString(const QString &value)
{
	const QByteArray utf8 = value.toUtf8();
	return {utf8.constData(), static_cast<size_t>(utf8.size())};
}

QString NewId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void AddBeforeVerticalSpacer(QVBoxLayout *layout, QWidget *widget)
{
	if (!layout || !widget) {
		return;
	}

	for (int index = 0; index < layout->count(); ++index) {
		QLayoutItem *item = layout->itemAt(index);
		if (item && item->spacerItem() &&
		    item->spacerItem()->sizePolicy().verticalPolicy() == QSizePolicy::Expanding) {
			layout->insertWidget(index, widget);
			return;
		}
	}

	layout->addWidget(widget);
}

void ConfigureEmbeddedPropertiesView(OBSPropertiesView *view)
{
	if (!view) {
		return;
	}

	view->setFrameShape(QFrame::NoFrame);
	view->setScrolling(false);
	view->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	QObject::connect(view, &OBSPropertiesView::PropertiesRefreshed, view, [view]() {
		view->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
		view->updateGeometry();
	});
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

QWidget *MovePageContentsToTab(QWidget *page, const QString &title, QTabWidget *&tabs)
{
	auto *scroll = OBSNativeSettingsPage::MoveContentsToScroll(page);
	if (!scroll || !page || !page->layout()) {
		return nullptr;
	}
	QLayout *pageLayout = page->layout();

	tabs = new QTabWidget(page);
	pageLayout->addWidget(tabs);
	tabs->addTab(scroll, title);
	return scroll ? scroll->widget() : nullptr;
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

bool IsValidCanvasVideoInfo(const obs_video_info &info)
{
	return info.base_width >= 32 && info.base_height >= 32 && info.output_width >= 32 && info.output_height >= 32 &&
	       info.fps_num > 0 && info.fps_den > 0 && info.output_format != VIDEO_FORMAT_NONE;
}

bool CanvasVideoInfoEqual(const obs_video_info &left, const obs_video_info &right)
{
	return left.fps_num == right.fps_num && left.fps_den == right.fps_den && left.base_width == right.base_width &&
	       left.base_height == right.base_height && left.output_width == right.output_width &&
	       left.output_height == right.output_height && left.output_format == right.output_format &&
	       left.adapter == right.adapter && left.gpu_conversion == right.gpu_conversion &&
	       left.colorspace == right.colorspace && left.range == right.range && left.scale_type == right.scale_type;
}

const char *ScaleTypeConfigValue(enum obs_scale_type scaleType)
{
	switch (scaleType) {
	case OBS_SCALE_BILINEAR:
		return "bilinear";
	case OBS_SCALE_AREA:
		return "area";
	case OBS_SCALE_LANCZOS:
		return "lanczos";
	case OBS_SCALE_BICUBIC:
	default:
		return "bicubic";
	}
}

void SaveMainCanvasVideoConfig(config_t *config, const obs_video_info &info)
{
	config_set_uint(config, "Video", "BaseCX", info.base_width);
	config_set_uint(config, "Video", "BaseCY", info.base_height);
	config_set_uint(config, "Video", "OutputCX", info.output_width);
	config_set_uint(config, "Video", "OutputCY", info.output_height);
	config_set_string(config, "Video", "ScaleType", ScaleTypeConfigValue(info.scale_type));
	config_set_uint(config, "Video", "FPSInt", info.fps_num / std::max(1U, info.fps_den));
	config_set_uint(config, "Video", "FPSNum", info.fps_num);
	config_set_uint(config, "Video", "FPSDen", info.fps_den);

	static constexpr const char *commonFps[] = {"10", "20", "24 NTSC", "25 PAL", "29.97",
						    "30", "48", "50 PAL",  "59.94",  "60"};
	for (const char *value : commonFps) {
		uint32_t numerator = 0;
		uint32_t denominator = 0;
		if (ParseCommonFps(QString::fromUtf8(value), numerator, denominator) && numerator == info.fps_num &&
		    denominator == info.fps_den) {
			config_set_uint(config, "Video", "FPSType", 0);
			config_set_string(config, "Video", "FPSCommon", value);
			return;
		}
	}

	if (info.fps_den == 1) {
		config_set_uint(config, "Video", "FPSType", 1);
	} else {
		config_set_uint(config, "Video", "FPSType", 2);
	}
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
		bool main = false;
	};

	struct DestinationUi {
		QString id;
		QWidget *page = nullptr;
		QCheckBox *enabled = nullptr;
		QLineEdit *name = nullptr;
		QComboBox *session = nullptr;
		QComboBox *output = nullptr;
		QComboBox *serviceType = nullptr;
		QSpinBox *priority = nullptr;
		QCheckBox *dynamicBitrate = nullptr;
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
		QButtonGroup *audioMix = nullptr;
		QComboBox *failoverMode = nullptr;
		QVBoxLayout *videoPropertiesLayout = nullptr;
		QVBoxLayout *audioPropertiesLayout = nullptr;
		OBSPropertiesView *videoProperties = nullptr;
		OBSPropertiesView *audioProperties = nullptr;
		QSpinBox *videoBitrateOverride = nullptr;
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
	bool videoResetRequired = false;

	RouteSet routes;
	OBS::Output::SessionSet sessions;
	std::vector<CanvasDraft> canvasDrafts;
	CanvasDraft mainCanvasDraft;
	std::set<QString> deletedCanvasUuids;

	QTabWidget *destinationTabs = nullptr;
	QTabWidget *outputTabs = nullptr;
	QTabWidget *canvasTabs = nullptr;
	QScrollArea *nativeStreamPage = nullptr;
	QScrollArea *nativeVideoPage = nullptr;
	QComboBox *outputMode = nullptr;
	std::vector<QComboBox *> replayPrograms;
	QString mainCanvasOriginalName;
	QString mainCanvasDraftName;

	std::vector<std::unique_ptr<DestinationUi>> destinationUis;
	std::unique_ptr<DestinationUi> primaryDestinationUi;
	QWidget *primaryDestinationExtras = nullptr;
	Destination primaryDestination;
	std::vector<std::unique_ptr<RouteUi>> routeUis;
	std::vector<std::unique_ptr<CanvasUi>> canvasUis;
	std::unique_ptr<CanvasUi> mainCanvasUi;

	void UpdateOutputTabVisibility()
	{
		if (!outputTabs || !outputMode) {
			return;
		}

		const int outputModeIndex = outputMode->currentIndex();
		int firstVisibleTab = -1;
		for (int index = 0; index < outputTabs->count(); ++index) {
			QWidget *page = outputTabs->widget(index);
			const QVariant nativeMode = page ? page->property("outputMode") : QVariant{};
			const bool visible = !nativeMode.isValid() || nativeMode.toInt() == outputModeIndex;
			outputTabs->setTabVisible(index, visible);
			if (visible && firstVisibleTab < 0) {
				firstVisibleTab = index;
			}
		}

		const int currentIndex = outputTabs->currentIndex();
		if (firstVisibleTab >= 0 && (currentIndex < 0 || !outputTabs->isTabVisible(currentIndex))) {
			outputTabs->setCurrentIndex(firstVisibleTab);
		}
	}

	void BuildUnifiedOutputTabs(QWidget *outputPage)
	{
		outputMode = outputPage->findChild<QComboBox *>(QStringLiteral("outputMode"));
		auto *outputModePages = outputPage->findChild<QStackedWidget *>(QStringLiteral("outputModePages"));
		auto *simplePage = outputPage->findChild<QWidget *>(QStringLiteral("easyOutputsPage"));
		auto *advancedPage = outputPage->findChild<QWidget *>(QStringLiteral("advOutputsPage"));
		auto *nativeTabs = outputPage->findChild<QTabWidget *>(QStringLiteral("advOutTabs"));

		if (!outputModePages || !simplePage || !advancedPage || !nativeTabs) {
			MovePageContentsToTab(outputPage, QTStr("Basic.Settings.Output"), outputTabs);
			return;
		}

		QLayout *parentLayout = outputModePages->parentWidget() ? outputModePages->parentWidget()->layout()
									: nullptr;
		const int originalIndex = parentLayout ? parentLayout->indexOf(outputModePages) : -1;
		if (parentLayout) {
			parentLayout->removeWidget(outputModePages);
		}
		outputModePages->hide();

		outputTabs = new QTabWidget(outputPage);
		if (auto *boxLayout = qobject_cast<QBoxLayout *>(parentLayout); boxLayout && originalIndex >= 0) {
			boxLayout->insertWidget(originalIndex, outputTabs);
		} else if (outputPage->layout()) {
			outputPage->layout()->addWidget(outputTabs);
		}

		outputModePages->removeWidget(simplePage);
		outputModePages->removeWidget(advancedPage);

		auto addNativeTab = [this](QWidget *page, const QIcon &icon, const QString &title, int mode) {
			if (!page) {
				return;
			}
			page->setParent(outputTabs);
			page->setProperty("outputMode", mode);
			outputTabs->addTab(page, icon, title);
		};

		addNativeTab(simplePage, {}, QTStr("Basic.Settings.Output"), 0);
		while (nativeTabs->count() > 0) {
			const QString title = nativeTabs->tabText(0);
			const QIcon icon = nativeTabs->tabIcon(0);
			const QString toolTip = nativeTabs->tabToolTip(0);
			const QString whatsThis = nativeTabs->tabWhatsThis(0);
			QWidget *page = nativeTabs->widget(0);
			nativeTabs->removeTab(0);
			addNativeTab(page, icon, title, 1);
			const int index = outputTabs->count() - 1;
			outputTabs->setTabToolTip(index, toolTip);
			outputTabs->setTabWhatsThis(index, whatsThis);
		}
		nativeTabs->hide();

		if (outputMode) {
			QObject::connect(outputMode, &QComboBox::currentIndexChanged, outputPage,
					 [this](int) { UpdateOutputTabVisibility(); });
		}
		UpdateOutputTabVisibility();
	}

	void RemovePrimaryDestinationExtras()
	{
		if (!primaryDestinationExtras) {
			return;
		}
		if (QWidget *parent = primaryDestinationExtras->parentWidget(); parent && parent->layout()) {
			parent->layout()->removeWidget(primaryDestinationExtras);
		}
		delete primaryDestinationExtras;
		primaryDestinationExtras = nullptr;
	}

	Impl(OBSBasic *main_, QWidget *streamPage, QWidget *outputPage, QWidget *videoPage,
	     std::function<void()> changed)
		: main(main_),
		  changedCallback(std::move(changed))
	{
		nativeStreamPage = OBSNativeSettingsPage::MoveContentsToScroll(streamPage);
		destinationTabs = new QTabWidget(streamPage);
		streamPage->layout()->addWidget(destinationTabs);
		destinationTabs->addTab(nativeStreamPage, QTStr("OBSPro.Settings.Stream.Primary"));
		BuildUnifiedOutputTabs(outputPage);
		nativeVideoPage = OBSNativeSettingsPage::MoveContentsToScroll(videoPage);
		canvasTabs = new QTabWidget(videoPage);
		videoPage->layout()->addWidget(canvasTabs);

		auto addReplayProgram = [this](QFormLayout *form) {
			if (!form || !form->parentWidget()) {
				return;
			}
			QWidget *parent = form->parentWidget();
			auto *combo = new QComboBox(parent);
			form->insertRow(0,
					OBSNativeSettingsPage::CreateLabel(
						parent, QTStr("OBSPro.Settings.Replay.Source"), combo),
					combo);
			auto *description = new QLabel(QTStr("OBSPro.Settings.Replay.Description"), parent);
			description->setWordWrap(true);
			form->insertRow(1, QString(), description);
			replayPrograms.emplace_back(combo);
			QObject::connect(combo, &QComboBox::currentIndexChanged, parent, [this, combo](int) {
				if (loading) {
					return;
				}
				const QString selected = combo->currentData().toString();
				for (QComboBox *other : replayPrograms) {
					if (other == combo) {
						continue;
					}
					const QSignalBlocker blocker(other);
					const int index = other->findData(selected);
					other->setCurrentIndex(index >= 0 ? index : 0);
				}
				MarkChanged();
			});
		};
		addReplayProgram(outputPage->findChild<QFormLayout *>(QStringLiteral("formLayout_24")));
		addReplayProgram(outputPage->findChild<QFormLayout *>(QStringLiteral("formLayout_30")));

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
	}

	void MarkChanged()
	{
		if (!loading && changedCallback) {
			changedCallback();
		}
	}

	static QString CurrentTabId(QTabWidget *tabs, const char *property, const QString &nativeId = {})
	{
		if (!tabs || tabs->currentIndex() < 0) {
			return {};
		}
		QWidget *page = tabs->currentWidget();
		const QVariant value = page ? page->property(property) : QVariant{};
		return value.isValid() ? value.toString() : nativeId;
	}

	static void RestoreTabId(QTabWidget *tabs, const char *property, const QString &id,
				 const QString &nativeId = {})
	{
		if (!tabs || id.isEmpty()) {
			return;
		}
		if (!nativeId.isEmpty() && id == nativeId && tabs->count() > 0) {
			for (int index = 0; index < tabs->count(); ++index) {
				if (tabs->isTabVisible(index)) {
					tabs->setCurrentIndex(index);
					return;
				}
			}
			return;
		}
		for (int index = 0; index < tabs->count(); ++index) {
			QWidget *page = tabs->widget(index);
			if (page && page->property(property).toString() == id) {
				tabs->setCurrentIndex(index);
				return;
			}
		}
	}

	Route *FindRoute(const QString &id)
	{
		auto found = std::find_if(routes.routes.begin(), routes.routes.end(),
					  [&](const Route &route) { return route.id == ToStdString(id); });
		return found == routes.routes.end() ? nullptr : &*found;
	}

	OBS::Output::PlatformSessionConfig *FindSession(const std::string &id)
	{
		const auto found = std::find_if(sessions.sessions.begin(), sessions.sessions.end(),
						[&](const auto &session) { return session.id == id; });
		return found == sessions.sessions.end() ? nullptr : &*found;
	}

	Route *PrimaryRoute()
	{
		auto found = std::find_if(routes.routes.begin(), routes.routes.end(),
					  [](const Route &route) { return route.primary; });
		return found == routes.routes.end() ? nullptr : &*found;
	}

	std::pair<Route *, Destination *> FindDestination(const QString &id)
	{
		if (id == QString::fromUtf8(PrimaryDestinationId)) {
			return {PrimaryRoute(), &primaryDestination};
		}
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

	CanvasDraft *FindCanvasDraftForUi(const QString &id)
	{
		return id == QStringLiteral("main") ? &mainCanvasDraft : FindCanvasDraft(id);
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
		primary->name = ToStdString(QTStr("OBSPro.Settings.Output.Upstream"));
		primary->canvas = OBS::Output::CanvasReferenceFromCanvas(mainCanvas);
		primary->enabled = true;
		routes.schemaVersion = OBS::Output::RouteSchemaVersion;
	}

	void LoadRoutes()
	{
		routes = {};
		sessions = {};
		const char *serializedSessions = config_get_string(main->Config(), "Stream1", "PlatformSessions");
		const char *serializedRoutes = config_get_string(main->Config(), "Stream1", "OutputRoutes");
		bool loaded = false;
		if (serializedSessions && *serializedSessions) {
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
			} else {
				sessions = OBS::Output::MigrateRouteSet(routes);
			}
		}
		EnsurePrimaryRoute();
		sessions = OBS::Output::ReconcileRouteSet(sessions, routes);
		routes = OBS::Output::ToRouteSet(sessions);
		EnsurePrimaryRoute();
	}

	void LoadPrimaryDestination()
	{
		primaryDestination = {};
		primaryDestination.id = PrimaryDestinationId;
		primaryDestination.sessionId = "stream1";
		primaryDestination.name = QTStr("OBSPro.Settings.Stream.Primary").toStdString();
		if (const auto *session = FindSession("stream1")) {
			primaryDestination.name = session->name;
			primaryDestination.enabled = session->enabled;
			primaryDestination.dynamicBitrateEnabled = session->dynamicBitrateEnabled;
		}

		obs_service_t *service = main->GetService();
		if (!service) {
			return;
		}
		primaryDestination.service = obs_service_get_type(service);
		OBSDataAutoRelease settings = obs_service_get_settings(service);
		primaryDestination.serviceSettingsJson = SettingsJson(settings);
		primaryDestination.serviceName = obs_data_get_string(settings, "service");
		primaryDestination.server = obs_data_get_string(settings, "server");
		primaryDestination.streamKey = obs_data_get_string(settings, "key");
		primaryDestination.useAuthentication = obs_data_get_bool(settings, "use_auth");
		primaryDestination.username = obs_data_get_string(settings, "username");
		primaryDestination.password = obs_data_get_string(settings, "password");
	}

	void PopulateReplayProgramCombo(bool usePersistedSelection = false)
	{
		if (replayPrograms.empty()) {
			return;
		}
		const QString selected = !usePersistedSelection && replayPrograms.front()->count() > 0
						 ? replayPrograms.front()->currentData().toString()
						 : QString::fromUtf8(sessions.replayBuffer.programId.c_str());
		for (QComboBox *combo : replayPrograms) {
			const QSignalBlocker blocker(combo);
			combo->clear();
			combo->addItem(QTStr("OBSPro.Settings.Replay.Legacy"), QString());
			for (const Route &route : routes.routes) {
				if (route.enabled && route.kind == Kind::Stream) {
					combo->addItem(QString::fromUtf8(route.name.c_str()),
						       QString::fromUtf8(route.id.c_str()));
				}
			}
			const int index = combo->findData(selected);
			combo->setCurrentIndex(index >= 0 ? index : 0);
		}
	}

	void LoadCanvases()
	{
		canvasDrafts.clear();
		mainCanvasDraft = {};
		deletedCanvasUuids.clear();
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		mainCanvasOriginalName = mainCanvas ? QString::fromUtf8(obs_canvas_get_name(mainCanvas))
						    : QTStr("OBSPro.Settings.Canvas.Main");
		mainCanvasDraftName = mainCanvasOriginalName;
		mainCanvasDraft.id = QStringLiteral("main");
		mainCanvasDraft.uuid = mainCanvas ? QString::fromUtf8(obs_canvas_get_uuid(mainCanvas)) : QString{};
		mainCanvasDraft.name = mainCanvasDraftName;
		mainCanvasDraft.originalName = mainCanvasDraftName;
		mainCanvasDraft.existing = true;
		mainCanvasDraft.main = true;
		if (!mainCanvas || !obs_canvas_get_video_info(mainCanvas, &mainCanvasDraft.info) ||
		    !IsValidCanvasVideoInfo(mainCanvasDraft.info)) {
			obs_video_info mainInfo{};
			if (obs_get_video_info(&mainInfo) && IsValidCanvasVideoInfo(mainInfo)) {
				mainCanvasDraft.info = mainInfo;
			}
		}
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
			if (!obs_canvas_get_video_info(canvas, &draft.info) || !IsValidCanvasVideoInfo(draft.info)) {
				obs_video_info mainInfo{};
				if (obs_get_video_info(&mainInfo) && IsValidCanvasVideoInfo(mainInfo)) {
					draft.info = mainInfo;
				}
			}
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
		combo->addItem(mainCanvasDraftName.isEmpty() ? QTStr("OBSPro.Settings.Canvas.Main")
							     : mainCanvasDraftName,
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

	void PopulateSessionCombo(QComboBox *combo, const Destination &destination, std::string_view programId)
	{
		QSignalBlocker blocker(combo);
		combo->clear();
		const QString independentId =
			QStringLiteral("session-%1").arg(QString::fromUtf8(destination.id.c_str()));
		combo->addItem(QTStr("OBSPro.Settings.Session.Independent"), independentId);
		for (const auto &session : sessions.sessions) {
			const bool boundToProgram =
				std::any_of(session.programBindings.begin(), session.programBindings.end(),
					    [&](const auto &binding) { return binding.programId == programId; });
			if (session.compatibilityDefault || session.id == ToStdString(independentId) ||
			    !boundToProgram) {
				continue;
			}
			combo->addItem(QString::fromUtf8(session.name.c_str()), QString::fromUtf8(session.id.c_str()));
		}

		const QString selected = QString::fromUtf8(destination.sessionId.c_str());
		int index = combo->findData(selected);
		if (index < 0 && !selected.isEmpty()) {
			combo->addItem(QString::fromUtf8(destination.name.c_str()), selected);
			index = combo->count() - 1;
		}
		combo->setCurrentIndex(std::max(0, index));
	}

	void SyncDestinationUi(DestinationUi &ui)
	{
		auto [route, destination] = FindDestination(ui.id);
		if (!route || !destination) {
			return;
		}
		destination->enabled = ui.enabled->isChecked();
		destination->name = ToStdString(ui.name->text().trimmed());
		destination->sessionId = ToStdString(ui.session->currentData().toString());
		destination->priority = static_cast<uint32_t>(ui.priority->value());
		destination->dynamicBitrateEnabled = ui.dynamicBitrate->isChecked();
		if (auto *session = FindSession(destination->sessionId)) {
			session->enabled = ui.enabled->isChecked();
			session->name = destination->name;
			session->dynamicBitrateEnabled = destination->dynamicBitrateEnabled;
		}
		if (ui.serviceType) {
			destination->service = ToStdString(ui.serviceType->currentData().toString());
		}
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
		ConfigureEmbeddedPropertiesView(ui.properties);
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
		const std::string id = ToStdString(destinationId);
		const auto source = std::find_if(sourceRoute->destinations.begin(), sourceRoute->destinations.end(),
						 [&](const Destination &item) { return item.id == id; });
		if (source == sourceRoute->destinations.end()) {
			return;
		}
		Destination moved = std::move(*source);
		sourceRoute->destinations.erase(source);
		targetRoute->destinations.emplace_back(std::move(moved));
	}

	void BuildDestinationTabs()
	{
		RemoveCustomTabs(destinationTabs, "outputDestinationId");
		destinationUis.clear();
		primaryDestinationUi.reset();
		RemovePrimaryDestinationExtras();
		if (Route *primary = PrimaryRoute()) {
			BuildDestinationEditor(*primary, primaryDestination, true);
		}
		for (Route &route : routes.routes) {
			for (Destination &destination : route.destinations) {
				BuildDestinationEditor(route, destination, false);
			}
		}
	}

	void BuildDestinationEditor(Route &route, Destination &destination, bool primary)
	{
		auto ui = std::make_unique<DestinationUi>();
		ui->id = QString::fromUtf8(destination.id.c_str());
		if (primary) {
			ui->page = nativeStreamPage;
			primaryDestinationExtras = new QWidget(nativeStreamPage->widget());
		} else {
			ui->page = new QWidget(destinationTabs);
			ui->page->setProperty("outputDestinationId", ui->id);
		}
		QWidget *contents = nullptr;
		QVBoxLayout *layout = nullptr;
		if (primary) {
			OBSNativeSettingsPage page(primaryDestinationExtras, false);
			contents = page.Contents();
			layout = page.Layout();
			AddBeforeVerticalSpacer(qobject_cast<QVBoxLayout *>(nativeStreamPage->widget()->layout()),
						primaryDestinationExtras);
		} else {
			OBSNativeSettingsPage page(ui->page);
			contents = page.Contents();
			layout = page.Layout();
		}
		auto *settings = new QGroupBox(primary ? QTStr("OBSPro.Settings.Session.Controls")
						       : QTStr("Basic.Settings.Stream.Destination"),
					       contents);
		settings->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		auto *form = new QFormLayout(settings);
		OBSNativeSettingsPage::ConfigureForm(form);
		layout->addWidget(settings);

		ui->enabled = new QCheckBox(QTStr("OBSPro.OutputRoutes.Enabled"), settings);
		const auto *session = FindSession(destination.sessionId);
		ui->enabled->setChecked(session ? session->enabled : destination.enabled);
		OBSNativeSettingsPage::AddRow(form, settings, QString(), ui->enabled);
		ui->name = new QLineEdit(QString::fromUtf8(destination.name.c_str()), settings);
		OBSNativeSettingsPage::AddRow(form, settings, QTStr("OBSPro.OutputRoutes.Name"), ui->name);
		ui->session = new QComboBox(settings);
		PopulateSessionCombo(ui->session, destination, route.id);
		OBSNativeSettingsPage::AddRow(form, settings, QTStr("OBSPro.Settings.Session.PlatformSession"),
					      ui->session);
		ui->output = new QComboBox(settings);
		PopulateOutputCombo(ui->output, QString::fromUtf8(route.id.c_str()));
		ui->output->setEnabled(!primary);
		OBSNativeSettingsPage::AddRow(form, settings, QTStr("OBSPro.Settings.Stream.EncodedOutput"),
					      ui->output);
		if (!primary) {
			ui->serviceType = new QComboBox(settings);
			PopulateServiceCombo(ui->serviceType, destination.service);
			OBSNativeSettingsPage::AddRow(form, settings, QTStr("Basic.AutoConfig.StreamPage.Service"),
						      ui->serviceType);
		}
		ui->priority = new QSpinBox(settings);
		ui->priority->setRange(0, 999);
		ui->priority->setValue(static_cast<int>(destination.priority));
		OBSNativeSettingsPage::AddRow(form, settings, QTStr("OBSPro.OutputRoutes.Priority"), ui->priority);
		ui->dynamicBitrate = new QCheckBox(QTStr("Basic.Settings.Output.DynamicBitrate"), settings);
		ui->dynamicBitrate->setChecked(session ? session->dynamicBitrateEnabled
						       : destination.dynamicBitrateEnabled);
		OBSNativeSettingsPage::AddRow(form, settings, QString(), ui->dynamicBitrate);

		if (!primary) {
			auto *properties = new QGroupBox(QTStr("OBSPro.Settings.Stream.ServiceSettings"), contents);
			properties->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
			ui->propertiesLayout = new QVBoxLayout(properties);
			ui->propertiesLayout->setContentsMargins(9, 2, 9, 9);
			layout->addWidget(properties);
		}
		layout->addStretch();
		auto *sessionControls = new QWidget(contents);
		auto *sessionControlsLayout = new QHBoxLayout(sessionControls);
		sessionControlsLayout->setContentsMargins(0, 0, 0, 0);
		auto *start = new QPushButton(QTStr("OBSPro.Settings.Session.Start"), sessionControls);
		auto *stop = new QPushButton(QTStr("OBSPro.Settings.Session.Stop"), sessionControls);
		auto *remove = new QPushButton(QTStr("OBSPro.Settings.Stream.RemoveDestination"), sessionControls);
		sessionControlsLayout->addWidget(start);
		sessionControlsLayout->addWidget(stop);
		sessionControlsLayout->addStretch();
		sessionControlsLayout->addWidget(remove);
		layout->addWidget(sessionControls);
		remove->setVisible(!primary);

		DestinationUi *raw = ui.get();
		QObject::connect(ui->enabled, &QCheckBox::toggled, ui->page, [this]() { MarkChanged(); });
		QObject::connect(ui->name, &QLineEdit::textChanged, ui->page, [this, raw](const QString &text) {
			auto [currentRoute, current] = FindDestination(raw->id);
			if (currentRoute && current) {
				current->name = ToStdString(text.trimmed());
			}
			destinationTabs->setTabText(destinationTabs->indexOf(raw->page), text.trimmed());
			RefreshAssignedDestinationLabels();
			MarkChanged();
		});
		QObject::connect(ui->priority, &QSpinBox::valueChanged, ui->page, [this](int) { MarkChanged(); });
		QObject::connect(ui->session, &QComboBox::currentIndexChanged, ui->page, [this, raw](int) {
			const auto *selected = FindSession(ToStdString(raw->session->currentData().toString()));
			if (selected) {
				const QSignalBlocker blocker(raw->dynamicBitrate);
				raw->dynamicBitrate->setChecked(selected->dynamicBitrateEnabled);
			}
			SyncDestinationUi(*raw);
			MarkChanged();
		});
		QObject::connect(ui->dynamicBitrate, &QCheckBox::toggled, ui->page, [this](bool) { MarkChanged(); });
		QObject::connect(ui->output, &QComboBox::currentIndexChanged, ui->page, [this, raw](int) {
			SyncDestinationUi(*raw);
			const QString targetProgram = raw->output->currentData().toString();
			auto [currentRoute, current] = FindDestination(raw->id);
			if (currentRoute && current && currentRoute->id != ToStdString(targetProgram)) {
				const bool sharesSession = std::any_of(
					routes.routes.begin(), routes.routes.end(), [&](const Route &route) {
						return std::any_of(route.destinations.begin(), route.destinations.end(),
								   [&](const Destination &destination) {
									   return destination.id != current->id &&
										  destination.sessionId ==
											  current->sessionId;
								   });
					});
				if (sharesSession) {
					current->sessionId = "session-" + current->id;
				}
			}
			MoveDestination(raw->id, targetProgram);
			auto [newRoute, moved] = FindDestination(raw->id);
			if (newRoute && moved) {
				PopulateSessionCombo(raw->session, *moved, newRoute->id);
			}
			RefreshAssignedDestinationLabels();
			MarkChanged();
		});
		if (ui->serviceType) {
			QObject::connect(ui->serviceType, &QComboBox::currentIndexChanged, ui->page, [this, raw](int) {
				auto [currentRoute, current] = FindDestination(raw->id);
				if (!currentRoute || !current) {
					return;
				}
				current->service = ToStdString(raw->serviceType->currentData().toString());
				current->serviceSettingsJson.clear();
				current->server.clear();
				current->streamKey.clear();
				CreateServiceProperties(*raw);
				MarkChanged();
			});
		}
		QObject::connect(remove, &QPushButton::clicked, ui->page,
				 [this, id = ui->id]() { RemoveDestination(id); });
		QObject::connect(start, &QPushButton::clicked, ui->page, [this, raw]() {
			main->StartPlatformSession(ToStdString(raw->session->currentData().toString()));
		});
		QObject::connect(stop, &QPushButton::clicked, ui->page, [this, raw]() {
			main->StopPlatformSession(ToStdString(raw->session->currentData().toString()));
		});

		const QString title = QString::fromUtf8(destination.name.c_str());
		if (primary) {
			primaryDestinationUi = std::move(ui);
			destinationTabs->setTabText(destinationTabs->indexOf(primaryDestinationUi->page), title);
		} else {
			destinationUis.emplace_back(std::move(ui));
			destinationTabs->addTab(destinationUis.back()->page, title);
			CreateServiceProperties(*destinationUis.back());
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
		destination.sessionId = "session-" + destination.id;
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
		if (ui.videoBitrateOverride) {
			route->videoBitrateOverride = static_cast<uint32_t>(ui.videoBitrateOverride->value());
		}
		route->audioMix = static_cast<uint32_t>(std::max(0, ui.audioMix->checkedId()));
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
		if (video && ui.videoBitrateOverride) {
			delete ui.videoBitrateOverride;
			ui.videoBitrateOverride = nullptr;
		}
		const std::string &encoderId = video ? route->videoEncoderId : route->audioEncoderId;
		const std::string &serialized = video ? route->videoEncoderSettingsJson
						      : route->audioEncoderSettingsJson;
		if (encoderId.empty()) {
			notice = new QLabel(QTStr("OBSPro.Settings.Output.InheritEncoderDescription"), ui.page);
			notice->setWordWrap(true);
			layout->addWidget(notice);

			if (video) {
				auto *form = new QFormLayout;
				OBSNativeSettingsPage::ConfigureForm(form);
				ui.videoBitrateOverride = new QSpinBox(ui.page);
				ui.videoBitrateOverride->setRange(0, 1000000);
				ui.videoBitrateOverride->setSingleStep(100);
				ui.videoBitrateOverride->setSuffix(QStringLiteral(" Kbps"));
				ui.videoBitrateOverride->setSpecialValueText(
					QTStr("OBSPro.Settings.Output.InheritBitrate"));
				ui.videoBitrateOverride->setValue(static_cast<int>(route->videoBitrateOverride));
				form->addRow(OBSNativeSettingsPage::CreateLabel(
						     ui.page, QTStr("Basic.Settings.Output.VideoBitrate"),
						     ui.videoBitrateOverride),
					     ui.videoBitrateOverride);
				layout->addLayout(form);
				QObject::connect(ui.videoBitrateOverride, &QSpinBox::valueChanged, ui.page,
						 [this](int) { MarkChanged(); });
			}
			return;
		}
		OBSDataAutoRelease defaults = obs_encoder_defaults(encoderId.c_str());
		OBSDataAutoRelease settings = SettingsFromJson(serialized, defaults);
		view = new OBSPropertiesView(settings.Get(), encoderId.c_str(),
					     (PropertiesReloadCallback)obs_get_encoder_properties, 170);
		ConfigureEmbeddedPropertiesView(view);
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
		PopulateReplayProgramCombo();
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
		int insertIndex = outputTabs->count();
		for (Route &route : routes.routes) {
			if (route.primary) {
				continue;
			}
			auto ui = std::make_unique<RouteUi>();
			ui->id = QString::fromUtf8(route.id.c_str());
			ui->page = new QWidget(outputTabs);
			ui->page->setProperty("outputRouteId", ui->id);
			OBSNativeSettingsPage page(ui->page);
			QWidget *contents = page.Contents();
			auto *contentsLayout = page.Layout();
			auto *settings = new QGroupBox(QTStr("Basic.Settings.Output.Adv.Streaming.Settings"), contents);
			settings->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
			auto *form = new QFormLayout(settings);
			OBSNativeSettingsPage::ConfigureForm(form);
			contentsLayout->addWidget(settings);

			ui->enabled = new QCheckBox(QTStr("OBSPro.OutputRoutes.Enabled"), settings);
			ui->enabled->setChecked(route.enabled);
			OBSNativeSettingsPage::AddRow(form, settings, QString(), ui->enabled);
			ui->name = new QLineEdit(QString::fromUtf8(route.name.c_str()), settings);
			OBSNativeSettingsPage::AddRow(form, settings, QTStr("OBSPro.OutputRoutes.Name"), ui->name);
			ui->canvas = new QComboBox(settings);
			PopulateCanvasCombo(ui->canvas, route.canvas);
			OBSNativeSettingsPage::AddRow(form, settings, QTStr("OBSPro.OutputRoutes.Canvas"), ui->canvas);
			ui->videoEncoder = new QComboBox(settings);
			PopulateEncoderCombo(ui->videoEncoder, OBS_ENCODER_VIDEO, route.videoEncoderId);
			OBSNativeSettingsPage::AddRow(form, settings, QTStr("Basic.Settings.Output.Encoder.Video"),
						      ui->videoEncoder);
			ui->audioEncoder = new QComboBox(settings);
			PopulateEncoderCombo(ui->audioEncoder, OBS_ENCODER_AUDIO, route.audioEncoderId);
			OBSNativeSettingsPage::AddRow(form, settings, QTStr("Basic.Settings.Output.Encoder.Audio"),
						      ui->audioEncoder);
			auto *audioTracks = new QWidget(settings);
			auto *audioTracksLayout = new QHBoxLayout(audioTracks);
			audioTracksLayout->setContentsMargins(0, 0, 0, 0);
			ui->audioMix = new QButtonGroup(audioTracks);
			for (int mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
				auto *button = new QRadioButton(QString::number(mix + 1), audioTracks);
				ui->audioMix->addButton(button, mix);
				audioTracksLayout->addWidget(button);
				button->setChecked(route.audioMix == static_cast<uint32_t>(mix));
			}
			audioTracksLayout->addStretch();
			OBSNativeSettingsPage::AddRow(form, settings, QTStr("OBSPro.Settings.Output.AudioMix"),
						      audioTracks);
			ui->failoverMode = new QComboBox(settings);
			ui->failoverMode->addItem(QTStr("OBSPro.Settings.Output.Parallel"),
						  static_cast<int>(FailoverMode::ClientParallel));
			ui->failoverMode->addItem(QTStr("OBSPro.Settings.Output.Sequential"),
						  static_cast<int>(FailoverMode::ClientSequential));
			ui->failoverMode->setCurrentIndex(
				std::max(0, ui->failoverMode->findData(static_cast<int>(route.failoverMode))));
			OBSNativeSettingsPage::AddRow(form, settings, QTStr("OBSPro.Settings.Output.DeliveryMode"),
						      ui->failoverMode);

			auto *videoGroup =
				new QGroupBox(QTStr("OBSPro.Settings.Output.VideoEncoderSettings"), contents);
			videoGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
			ui->videoPropertiesLayout = new QVBoxLayout(videoGroup);
			ui->videoPropertiesLayout->setContentsMargins(8, 2, 8, 8);
			contentsLayout->addWidget(videoGroup);
			auto *audioGroup =
				new QGroupBox(QTStr("OBSPro.Settings.Output.AudioEncoderSettings"), contents);
			audioGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
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
			QObject::connect(ui->audioMix, &QButtonGroup::idClicked, ui->page,
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
				if (!current->videoEncoderId.empty()) {
					current->videoBitrateOverride = 0;
				}
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
		if (sessions.replayBuffer.programId == ToStdString(id)) {
			sessions.replayBuffer.programId.clear();
		}
		BuildOutputTabs();
		MarkChanged();
	}

	void SyncCanvasUi(CanvasUi &ui)
	{
		CanvasDraft *draft = FindCanvasDraftForUi(ui.id);
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
		if (draft->main) {
			mainCanvasDraftName = draft->name;
			SyncMainCanvasToNative();
		}
	}

	void SyncMainCanvasToNative()
	{
		if (!nativeVideoPage) {
			return;
		}

		auto *baseResolution = nativeVideoPage->findChild<QComboBox *>(QStringLiteral("baseResolution"));
		auto *outputResolution = nativeVideoPage->findChild<QComboBox *>(QStringLiteral("outputResolution"));
		auto *downscaleFilter = nativeVideoPage->findChild<QComboBox *>(QStringLiteral("downscaleFilter"));
		auto *fpsType = nativeVideoPage->findChild<QComboBox *>(QStringLiteral("fpsType"));
		auto *fpsCommon = nativeVideoPage->findChild<QComboBox *>(QStringLiteral("fpsCommon"));
		auto *fpsInteger = nativeVideoPage->findChild<QSpinBox *>(QStringLiteral("fpsInteger"));
		auto *fpsNumerator = nativeVideoPage->findChild<QSpinBox *>(QStringLiteral("fpsNumerator"));
		auto *fpsDenominator = nativeVideoPage->findChild<QSpinBox *>(QStringLiteral("fpsDenominator"));
		if (baseResolution) {
			baseResolution->setCurrentText(
				ResolutionText(mainCanvasDraft.info.base_width, mainCanvasDraft.info.base_height));
		}
		if (outputResolution) {
			outputResolution->setCurrentText(
				ResolutionText(mainCanvasDraft.info.output_width, mainCanvasDraft.info.output_height));
		}
		if (downscaleFilter) {
			const int index = downscaleFilter->findData(static_cast<int>(mainCanvasDraft.info.scale_type));
			if (index >= 0) {
				downscaleFilter->setCurrentIndex(index);
			}
		}

		uint32_t commonNumerator = 0;
		uint32_t commonDenominator = 0;
		int selectedFpsType = 2;
		if (fpsCommon) {
			for (int index = 0; index < fpsCommon->count(); ++index) {
				if (ParseCommonFps(fpsCommon->itemText(index), commonNumerator, commonDenominator) &&
				    commonNumerator == mainCanvasDraft.info.fps_num &&
				    commonDenominator == mainCanvasDraft.info.fps_den) {
					fpsCommon->setCurrentIndex(index);
					selectedFpsType = 0;
					break;
				}
			}
		}
		if (selectedFpsType != 0 && mainCanvasDraft.info.fps_den == 1) {
			selectedFpsType = 1;
			if (fpsInteger) {
				fpsInteger->setValue(static_cast<int>(mainCanvasDraft.info.fps_num));
			}
		} else if (selectedFpsType == 2) {
			if (fpsNumerator) {
				fpsNumerator->setValue(static_cast<int>(mainCanvasDraft.info.fps_num));
			}
			if (fpsDenominator) {
				fpsDenominator->setValue(static_cast<int>(mainCanvasDraft.info.fps_den));
			}
		}
		if (fpsType) {
			fpsType->setCurrentIndex(selectedFpsType);
		}
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
		mainCanvasUi.reset();
		BuildNativeMainCanvasEditor();
		for (CanvasDraft &draft : canvasDrafts) {
			BuildCanvasEditor(draft);
		}
		RefreshRouteCanvasCombos();
	}

	void BuildNativeMainCanvasEditor()
	{
		if (!nativeVideoPage || !canvasTabs) {
			return;
		}

		auto ui = std::make_unique<CanvasUi>();
		ui->id = QStringLiteral("main");
		ui->page = nativeVideoPage;
		ui->baseResolution = nativeVideoPage->findChild<QComboBox *>(QStringLiteral("baseResolution"));
		ui->baseAspect = nativeVideoPage->findChild<QLabel *>(QStringLiteral("baseAspect"));
		ui->outputResolution = nativeVideoPage->findChild<QComboBox *>(QStringLiteral("outputResolution"));
		ui->outputAspect = nativeVideoPage->findChild<QLabel *>(QStringLiteral("scaledAspect"));
		ui->downscaleFilter = nativeVideoPage->findChild<QComboBox *>(QStringLiteral("downscaleFilter"));
		ui->fpsType = nativeVideoPage->findChild<QComboBox *>(QStringLiteral("fpsType"));
		ui->fpsTypes = nativeVideoPage->findChild<QStackedWidget *>(QStringLiteral("fpsTypes"));
		ui->fpsCommon = nativeVideoPage->findChild<QComboBox *>(QStringLiteral("fpsCommon"));
		ui->fpsInteger = nativeVideoPage->findChild<QSpinBox *>(QStringLiteral("fpsInteger"));
		ui->fpsNumerator = nativeVideoPage->findChild<QSpinBox *>(QStringLiteral("fpsNumerator"));
		ui->fpsDenominator = nativeVideoPage->findChild<QSpinBox *>(QStringLiteral("fpsDenominator"));

		if (!ui->baseResolution || !ui->outputResolution || !ui->downscaleFilter || !ui->fpsType ||
		    !ui->fpsTypes || !ui->fpsCommon || !ui->fpsInteger || !ui->fpsNumerator || !ui->fpsDenominator) {
			return;
		}

		QWidget *contents = nativeVideoPage->widget();
		if (!contents) {
			return;
		}

		ui->name = contents->findChild<QLineEdit *>(QStringLiteral("mainCanvasName"));
		if (!ui->name) {
			auto *general = contents->findChild<QGroupBox *>(QStringLiteral("videoGeneral"));
			auto *form = general ? general->findChild<QFormLayout *>(QStringLiteral("formLayout_15"))
					     : nullptr;
			if (!general || !form) {
				return;
			}
			ui->name = new QLineEdit(mainCanvasDraftName, general);
			ui->name->setObjectName(QStringLiteral("mainCanvasName"));
			form->insertRow(0,
					OBSNativeSettingsPage::CreateLabel(general, QTStr("OBSPro.OutputRoutes.Name"),
									   ui->name),
					ui->name);
		} else {
			ui->name->setText(mainCanvasDraftName);
		}

		if (!ui->name->property("obsProCanvasNameConnected").toBool()) {
			QObject::connect(ui->name, &QLineEdit::textChanged, ui->page, [this](const QString &text) {
				mainCanvasDraftName = text.trimmed();
				const int tabIndex = canvasTabs->indexOf(nativeVideoPage);
				if (tabIndex >= 0) {
					canvasTabs->setTabText(tabIndex, mainCanvasDraftName);
				}
				RefreshRouteCanvasCombos();
				MarkChanged();
			});
			ui->name->setProperty("obsProCanvasNameConnected", true);
		}

		const int tabIndex = canvasTabs->indexOf(nativeVideoPage);
		if (tabIndex < 0) {
			canvasTabs->addTab(nativeVideoPage, mainCanvasDraftName);
		} else {
			canvasTabs->setTabText(tabIndex, mainCanvasDraftName);
		}
		mainCanvasUi = std::move(ui);
	}

	void BuildCanvasEditor(CanvasDraft &draft)
	{
		auto ui = std::make_unique<CanvasUi>();
		ui->id = draft.id;
		ui->page = new QWidget(canvasTabs);
		ui->page->setProperty("canvasDraftId", draft.id);
		OBSNativeSettingsPage page(ui->page);
		QWidget *contents = page.Contents();
		auto *layout = page.Layout();
		auto *general = new QGroupBox(QTStr("Basic.Settings.General"), contents);
		general->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		auto *form = new QFormLayout(general);
		OBSNativeSettingsPage::ConfigureForm(form);
		layout->addWidget(general);
		ui->name = new QLineEdit(draft.name, general);
		OBSNativeSettingsPage::AddRow(form, general, QTStr("OBSPro.OutputRoutes.Name"), ui->name);

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
		form->addRow(OBSNativeSettingsPage::CreateLabel(general, QTStr("Basic.Settings.Video.BaseResolution"),
								ui->baseResolution),
			     baseResolutionLayout);
		form->addRow(OBSNativeSettingsPage::CreateLabel(general, QTStr("Basic.Settings.Video.ScaledResolution"),
								ui->outputResolution),
			     outputResolutionLayout);
		ui->downscaleFilter = new QComboBox(general);
		PopulateCanvasDownscaleFilter(ui->downscaleFilter, draft.info, ui->baseResolution->currentText(),
					      ui->outputResolution->currentText());
		OBSNativeSettingsPage::AddRow(form, general, QTStr("Basic.Settings.Video.DownscaleFilter"),
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
		     {QStringLiteral("10"), QStringLiteral("20"), QStringLiteral("24 NTSC"), QStringLiteral("25 PAL"),
		      QStringLiteral("29.97"), QStringLiteral("30"), QStringLiteral("48"), QStringLiteral("50 PAL"),
		      QStringLiteral("59.94"), QStringLiteral("60")}) {
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
			OBSNativeSettingsPage::AddRow(form, general, QTStr("OBSPro.Settings.Canvas.CreationMode"),
						      mode);
		}
		auto *description = new QLabel(QTStr("OBSPro.Settings.Canvas.SceneSetDescription"), contents);
		description->setWordWrap(true);
		layout->addWidget(description);
		layout->addStretch();
		QPushButton *remove = nullptr;
		remove = new QPushButton(QTStr("OBSPro.Settings.Canvas.Remove"), contents);
		layout->addWidget(remove, 0, Qt::AlignRight);

		CanvasUi *raw = ui.get();
		QObject::connect(ui->name, &QLineEdit::textChanged, ui->page, [this, raw](const QString &text) {
			if (CanvasDraft *draft = FindCanvasDraftForUi(raw->id)) {
				draft->name = text.trimmed();
				if (draft->main) {
					mainCanvasDraftName = draft->name;
				}
			}
			canvasTabs->setTabText(canvasTabs->indexOf(raw->page), text.trimmed());
			RefreshRouteCanvasCombos();
			SyncCanvasUi(*raw);
			MarkChanged();
		});
		QObject::connect(ui->baseResolution, &QComboBox::currentTextChanged, ui->page,
				 [this, raw](const QString &) {
					 UpdateAspectRatio(raw->baseResolution, raw->baseAspect);
					 if (CanvasDraft *draft = FindCanvasDraftForUi(raw->id)) {
						 PopulateCanvasDownscaleFilter(raw->downscaleFilter, draft->info,
									       raw->baseResolution->currentText(),
									       raw->outputResolution->currentText());
					 }
					 SyncCanvasUi(*raw);
					 MarkChanged();
				 });
		QObject::connect(ui->outputResolution, &QComboBox::currentTextChanged, ui->page,
				 [this, raw](const QString &) {
					 UpdateAspectRatio(raw->outputResolution, raw->outputAspect);
					 if (CanvasDraft *draft = FindCanvasDraftForUi(raw->id)) {
						 PopulateCanvasDownscaleFilter(raw->downscaleFilter, draft->info,
									       raw->baseResolution->currentText(),
									       raw->outputResolution->currentText());
					 }
					 SyncCanvasUi(*raw);
					 MarkChanged();
				 });
		QObject::connect(ui->fpsType, &QComboBox::currentIndexChanged, ui->page, [this, raw](int index) {
			raw->fpsTypes->setCurrentIndex(index);
			SyncCanvasUi(*raw);
			MarkChanged();
		});
		QObject::connect(ui->fpsCommon, &QComboBox::currentTextChanged, ui->page, [this, raw](const QString &) {
			SyncCanvasUi(*raw);
			MarkChanged();
		});
		QObject::connect(ui->fpsInteger, &QSpinBox::valueChanged, ui->page, [this, raw](int) {
			SyncCanvasUi(*raw);
			MarkChanged();
		});
		QObject::connect(ui->fpsNumerator, &QSpinBox::valueChanged, ui->page, [this, raw](int) {
			SyncCanvasUi(*raw);
			MarkChanged();
		});
		QObject::connect(ui->fpsDenominator, &QSpinBox::valueChanged, ui->page, [this, raw](int) {
			SyncCanvasUi(*raw);
			MarkChanged();
		});
		QObject::connect(ui->downscaleFilter, &QComboBox::currentIndexChanged, ui->page, [this, raw](int) {
			SyncCanvasUi(*raw);
			MarkChanged();
		});
		QObject::connect(remove, &QPushButton::clicked, ui->page, [this, id = ui->id]() { RemoveCanvas(id); });
		canvasTabs->addTab(ui->page, draft.name);
		canvasUis.emplace_back(std::move(ui));
	}

	QString UniqueCanvasName() const
	{
		QString base = QTStr("OBSPro.OutputRoutes.NewCanvas");
		QString candidate = base;
		int suffix = 2;
		auto exists = [&](const QString &name) {
			if (name == mainCanvasDraftName) {
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
		if (!obs_get_video_info(&info) || !IsValidCanvasVideoInfo(info)) {
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
		if (primaryDestinationUi) {
			SyncDestinationUi(*primaryDestinationUi);
		}
		for (auto &ui : destinationUis) {
			SyncDestinationUi(*ui);
		}
		for (auto &ui : routeUis) {
			SyncRouteUi(*ui);
		}
		if (mainCanvasUi) {
			SyncCanvasUi(*mainCanvasUi);
		}
		for (auto &ui : canvasUis) {
			SyncCanvasUi(*ui);
		}
	}

	bool Validate(QString &error)
	{
		SyncAll();
		if (primaryDestination.name.empty()) {
			error = QTStr("OBSPro.Settings.Stream.NameRequired");
			return false;
		}
		if (mainCanvasDraftName.isEmpty()) {
			error = QTStr("OBSPro.OutputRoutes.CanvasNameRequired");
			return false;
		}
		if (mainCanvasUi) {
			uint32_t width = 0;
			uint32_t height = 0;
			if (!ParseResolution(mainCanvasUi->baseResolution->currentText(), width, height) ||
			    !ParseResolution(mainCanvasUi->outputResolution->currentText(), width, height)) {
				error = QTStr("OBSPro.Settings.Canvas.InvalidVideo").arg(mainCanvasDraftName);
				return false;
			}
		}
		if (!IsValidCanvasVideoInfo(mainCanvasDraft.info)) {
			error = QTStr("OBSPro.Settings.Canvas.InvalidVideo").arg(mainCanvasDraftName);
			return false;
		}
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
				if (destination.enabled && destination.server.empty()) {
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
			canvasNames.emplace(ToStdString(mainCanvasDraftName));
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
			if (!IsValidCanvasVideoInfo(draft.info)) {
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

	bool MainCanvasVideoChanged() const
	{
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		obs_video_info current{};
		return !mainCanvas || !obs_canvas_get_video_info(mainCanvas, &current) ||
		       !CanvasVideoInfoEqual(current, mainCanvasDraft.info);
	}

	bool CanvasesChanged() const
	{
		if (!deletedCanvasUuids.empty() || mainCanvasDraftName != mainCanvasOriginalName ||
		    MainCanvasVideoChanged()) {
			return true;
		}
		for (const CanvasDraft &draft : canvasDrafts) {
			if (!draft.existing || draft.name != draft.originalName) {
				return true;
			}
			OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(ToStdString(draft.uuid).c_str());
			obs_video_info current{};
			if (!canvas || !obs_canvas_get_video_info(canvas, &current) ||
			    !CanvasVideoInfoEqual(current, draft.info)) {
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

		const RouteSet routesBefore = routes;
		const auto draftsBefore = canvasDrafts;
		const auto deletedBefore = deletedCanvasUuids;
		const QString mainNameBefore = mainCanvasOriginalName;
		videoResetRequired = false;

		struct CanvasState {
			QString uuid;
			QString name;
			obs_video_info info{};
		};
		std::vector<CanvasState> existingStates;
		existingStates.reserve(canvasDrafts.size());
		for (const CanvasDraft &draft : canvasDrafts) {
			if (!draft.existing) {
				continue;
			}
			OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(ToStdString(draft.uuid).c_str());
			CanvasState state;
			state.uuid = draft.uuid;
			if (!canvas || !obs_canvas_get_video_info(canvas, &state.info)) {
				error = QTStr("OBSPro.Settings.Canvas.Missing").arg(draft.name);
				return false;
			}
			state.name = QString::fromUtf8(obs_canvas_get_name(canvas));
			existingStates.emplace_back(std::move(state));
		}

		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		if (!mainCanvas) {
			error = QTStr("OBSPro.Settings.Canvas.Missing").arg(mainCanvasDraftName);
			return false;
		}
		obs_video_info mainInfo{};
		if (!obs_canvas_get_video_info(mainCanvas, &mainInfo)) {
			error = QTStr("OBSPro.Settings.Canvas.InvalidVideo").arg(mainCanvasDraftName);
			return false;
		}
		const bool mainVideoChanged = !CanvasVideoInfoEqual(mainInfo, mainCanvasDraft.info);
		struct MainVideoConfigState {
			uint32_t baseWidth = 0;
			uint32_t baseHeight = 0;
			uint32_t outputWidth = 0;
			uint32_t outputHeight = 0;
			uint32_t fpsType = 0;
			uint32_t fpsInt = 0;
			uint32_t fpsNum = 0;
			uint32_t fpsDen = 0;
			std::string fpsCommon;
			std::string scaleType;
		} mainVideoConfigBefore;
		config_t *config = main->Config();
		auto readConfigString = [config](const char *section, const char *name) {
			const char *value = config_get_string(config, section, name);
			return std::string(value ? value : "");
		};
		mainVideoConfigBefore.baseWidth = config_get_uint(config, "Video", "BaseCX");
		mainVideoConfigBefore.baseHeight = config_get_uint(config, "Video", "BaseCY");
		mainVideoConfigBefore.outputWidth = config_get_uint(config, "Video", "OutputCX");
		mainVideoConfigBefore.outputHeight = config_get_uint(config, "Video", "OutputCY");
		mainVideoConfigBefore.fpsType = config_get_uint(config, "Video", "FPSType");
		mainVideoConfigBefore.fpsInt = config_get_uint(config, "Video", "FPSInt");
		mainVideoConfigBefore.fpsNum = config_get_uint(config, "Video", "FPSNum");
		mainVideoConfigBefore.fpsDen = config_get_uint(config, "Video", "FPSDen");
		mainVideoConfigBefore.fpsCommon = readConfigString("Video", "FPSCommon");
		mainVideoConfigBefore.scaleType = readConfigString("Video", "ScaleType");
		QStringList deletedUuids;
		for (const QString &uuid : deletedCanvasUuids) {
			deletedUuids.push_back(uuid);
		}
		std::vector<obs_canvas_t *> createdCanvases;

		auto rollback = [&]() {
			routes = routesBefore;
			canvasDrafts = draftsBefore;
			deletedCanvasUuids = deletedBefore;
			mainCanvasOriginalName = mainNameBefore;
			videoResetRequired = false;
			config_set_uint(config, "Video", "BaseCX", mainVideoConfigBefore.baseWidth);
			config_set_uint(config, "Video", "BaseCY", mainVideoConfigBefore.baseHeight);
			config_set_uint(config, "Video", "OutputCX", mainVideoConfigBefore.outputWidth);
			config_set_uint(config, "Video", "OutputCY", mainVideoConfigBefore.outputHeight);
			config_set_uint(config, "Video", "FPSType", mainVideoConfigBefore.fpsType);
			config_set_uint(config, "Video", "FPSInt", mainVideoConfigBefore.fpsInt);
			config_set_uint(config, "Video", "FPSNum", mainVideoConfigBefore.fpsNum);
			config_set_uint(config, "Video", "FPSDen", mainVideoConfigBefore.fpsDen);
			config_set_string(config, "Video", "FPSCommon", mainVideoConfigBefore.fpsCommon.c_str());
			config_set_string(config, "Video", "ScaleType", mainVideoConfigBefore.scaleType.c_str());
			if (OBSCanvasAutoRelease currentMain = obs_get_main_canvas()) {
				obs_canvas_set_name(currentMain, ToStdString(mainNameBefore).c_str());
			}
			for (const CanvasState &state : existingStates) {
				OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(ToStdString(state.uuid).c_str());
				if (!canvas) {
					continue;
				}
				obs_canvas_set_name(canvas, ToStdString(state.name).c_str());
				obs_canvas_set_video_info(canvas, &state.info);
			}
			for (obs_canvas_t *canvas : createdCanvases) {
				if (canvas) {
					main->RemoveCanvas(OBSCanvas(canvas));
				}
			}
		};

		if (mainCanvas && mainCanvasDraftName != mainCanvasOriginalName) {
			obs_canvas_set_name(mainCanvas, ToStdString(mainCanvasDraftName).c_str());
			for (Route &route : routes.routes) {
				if (OBS::Output::CanvasReferenceMatches(route.canvas, mainCanvas)) {
					route.canvas = OBS::Output::CanvasReferenceFromCanvas(mainCanvas);
				}
			}
			mainCanvasOriginalName = mainCanvasDraftName;
		}
		if (mainVideoChanged) {
			SaveMainCanvasVideoConfig(main->Config(), mainCanvasDraft.info);
			videoResetRequired = true;
		}

		for (CanvasDraft &draft : canvasDrafts) {
			if (draft.existing) {
				OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(ToStdString(draft.uuid).c_str());
				if (!canvas) {
					error = QTStr("OBSPro.Settings.Canvas.Missing").arg(draft.name);
					rollback();
					return false;
				}
				obs_video_info current{};
				if (!obs_canvas_get_video_info(canvas, &current)) {
					error = QTStr("OBSPro.Settings.Canvas.InvalidVideo").arg(draft.name);
					rollback();
					return false;
				}
				if (draft.name != draft.originalName) {
					obs_canvas_set_name(canvas, ToStdString(draft.name).c_str());
				}
				const bool videoChanged = !CanvasVideoInfoEqual(current, draft.info);
				if (videoChanged && !obs_canvas_set_video_info(canvas, &draft.info)) {
					error = QTStr("OBSPro.OutputRoutes.CanvasResetFailed");
					rollback();
					return false;
				}
				videoResetRequired = videoResetRequired || videoChanged;
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
				rollback();
				return false;
			}
			createdCanvases.emplace_back(canvas);
			const bool duplicateLayout = draft.creationMode != CanvasCreationMode::Blank;
			const bool independentSources = draft.creationMode == CanvasCreationMode::IndependentSources;
			main->InitializeCanvasSceneSets(canvas, duplicateLayout, independentSources);
			ReplaceCanvasReference(pendingUuid, canvas);
			draft.uuid = QString::fromUtf8(obs_canvas_get_uuid(canvas));
			draft.id = draft.uuid;
			draft.existing = true;
			draft.originalName = draft.name;
		}

		if (videoResetRequired) {
			if (main->ResetVideo() != OBS_VIDEO_SUCCESS) {
				error = QTStr("OBSPro.OutputRoutes.CanvasResetFailed");
				rollback();
				return false;
			}
		}

		for (const QString &uuid : deletedUuids) {
			OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(ToStdString(uuid).c_str());
			if (canvas) {
				if (!main->RemoveCanvas(OBSCanvas(canvas))) {
					error = QTStr("OBSPro.Settings.Canvas.RemoveFailed").arg(uuid);
					return false;
				}
			}
		}
		deletedCanvasUuids.clear();
		main->SaveProject();
		main->RefreshCanvasTabs();
		return true;
	}

	bool Save(QString &error)
	{
		if (!Validate(error) || !ValidatePrimaryServiceSettings(error) || !ApplyCanvases(error)) {
			return false;
		}
		if (!ApplyPrimaryServiceSettings(error)) {
			return false;
		}
		const std::string serialized = OBS::Output::Serialize(routes);
		config_set_string(main->Config(), "Stream1", "OutputRoutes", serialized.c_str());
		if (!replayPrograms.empty()) {
			sessions.replayBuffer.programId = ToStdString(replayPrograms.front()->currentData().toString());
		}
		sessions = OBS::Output::ReconcileRouteSet(sessions, routes);
		const std::string serializedSessions = OBS::Output::Serialize(sessions);
		config_set_string(main->Config(), "Stream1", "PlatformSessions", serializedSessions.c_str());
		Load(true);
		return true;
	}

	bool ApplyPrimaryServiceSettings(QString &error)
	{
		/* The native Primary Stream page remains the source of truth for the
		 * platform, account, server, key, and service-specific settings. Its
		 * normal SaveStream1Settings path runs after this model is saved. */
		Q_UNUSED(error);
		return true;
	}

	bool ValidatePrimaryServiceSettings(QString &error) const
	{
		/* Validation of the native service fields is still performed by OBS'
		 * existing stream-settings path. */
		Q_UNUSED(error);
		return true;
	}

	void Load(bool preserveSelection = false)
	{
		const QString selectedDestination = preserveSelection
							    ? CurrentTabId(destinationTabs, "outputDestinationId",
									   QString::fromUtf8(PrimaryDestinationId))
							    : QString{};
		const QString selectedOutput =
			preserveSelection ? CurrentTabId(outputTabs, "outputRouteId", QStringLiteral("global"))
					  : QString{};
		const QString selectedCanvas =
			preserveSelection ? CurrentTabId(canvasTabs, "canvasDraftId", QStringLiteral("main"))
					  : QString{};
		loading = true;
		LoadRoutes();
		LoadPrimaryDestination();
		LoadCanvases();
		BuildDestinationTabs();
		BuildOutputTabs();
		BuildCanvasTabs();
		PopulateReplayProgramCombo(true);
		RestoreTabId(destinationTabs, "outputDestinationId", selectedDestination,
			     QString::fromUtf8(PrimaryDestinationId));
		RestoreTabId(outputTabs, "outputRouteId", selectedOutput, QStringLiteral("global"));
		RestoreTabId(canvasTabs, "canvasDraftId", selectedCanvas, QStringLiteral("main"));
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
