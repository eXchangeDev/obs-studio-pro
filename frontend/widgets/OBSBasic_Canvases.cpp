/******************************************************************************
    Copyright (C) 2025 by Dennis Sädtler <saedtler@twitch.tv>
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

#include "OBSBasic.hpp"

#include <QSignalBlocker>
#include <QTabBar>

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr const char *SceneSetIdKey = "multi_canvas_scene_set_id";
constexpr const char *SceneSetMainNameKey = "multi_canvas_scene_set_name";

std::string UniqueSourceName(const char *sourceName, const char *canvasName)
{
	const QString base =
		QStringLiteral("%1 — %2").arg(QString::fromUtf8(sourceName), QString::fromUtf8(canvasName));
	QString candidate = base;
	int suffix = 2;
	for (;;) {
		OBSSourceAutoRelease existing = obs_get_source_by_name(candidate.toUtf8().constData());
		if (!existing) {
			return candidate.toUtf8().constData();
		}
		candidate = QStringLiteral("%1 %2").arg(base).arg(suffix++);
	}
}

void SetSceneSetMetadata(obs_scene_t *scene, const char *sceneSetId, const char *mainName)
{
	if (!scene) {
		return;
	}
	OBSDataAutoRelease settings = obs_source_get_private_settings(obs_scene_get_source(scene));
	obs_data_set_string(settings, SceneSetIdKey, sceneSetId);
	obs_data_set_string(settings, SceneSetMainNameKey, mainName);
}

void CopySceneItemState(obs_sceneitem_t *source, obs_sceneitem_t *destination)
{
	obs_transform_info transform{};
	obs_sceneitem_get_info2(source, &transform);
	obs_sceneitem_set_info2(destination, &transform);

	obs_sceneitem_crop crop{};
	obs_sceneitem_get_crop(source, &crop);
	obs_sceneitem_set_crop(destination, &crop);
	obs_sceneitem_set_visible(destination, obs_sceneitem_visible(source));
	obs_sceneitem_set_locked(destination, obs_sceneitem_locked(source));
	obs_sceneitem_set_scale_filter(destination, obs_sceneitem_get_scale_filter(source));
	obs_sceneitem_set_blending_method(destination, obs_sceneitem_get_blending_method(source));
	obs_sceneitem_set_blending_mode(destination, obs_sceneitem_get_blending_mode(source));
	obs_sceneitem_set_order_position(destination, obs_sceneitem_get_order_position(source));

	OBSDataAutoRelease sourceSettings = obs_sceneitem_get_private_settings(source);
	OBSDataAutoRelease destinationSettings = obs_sceneitem_get_private_settings(destination);
	obs_data_apply(destinationSettings, sourceSettings);
}

} // namespace

void OBSBasic::CanvasRemoved(void *data, calldata_t *params)
{
	obs_canvas_t *canvas = static_cast<obs_canvas_t *>(calldata_ptr(params, "canvas"));
	QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), "RemoveCanvas", Q_ARG(OBSCanvas, OBSCanvas(canvas)));
}

const OBS::Canvas &OBSBasic::AddCanvas(const std::string &name, obs_video_info *ovi, int flags)
{
	OBSCanvas canvas = obs_canvas_create(name.c_str(), ovi, flags);
	auto &it = canvases.emplace_back(canvas);
	RefreshCanvasTabs();
	OnEvent(OBS_FRONTEND_EVENT_CANVAS_ADDED);
	return it;
}

bool OBSBasic::RemoveCanvas(OBSCanvas canvas)
{
	bool removed = false;
	if (!canvas) {
		return removed;
	}

	auto canvas_it = std::find(std::begin(canvases), std::end(canvases), canvas);
	if (canvas_it != std::end(canvases)) {
		// Move canvas to a temporary object to delay removal of the canvas and calls to its signal handlers
		// until after erase() completes. This is to avoid issues with recursion coming from the
		// CanvasRemoved() signal handler.
		OBS::Canvas tmp = std::move(*canvas_it);
		canvases.erase(canvas_it);
		removed = true;
	}

	if (removed) {
		RefreshCanvasTabs();
		OnEvent(OBS_FRONTEND_EVENT_CANVAS_REMOVED);
	}

	return removed;
}

void OBSBasic::ClearCanvases()
{
	// Delete canvases one-by-one to ensure OBS_FRONTEND_EVENT_CANVAS_REMOVED is sent for each
	while (!canvases.empty()) {
		RemoveCanvas(OBSCanvas(canvases.back()));
	}
}

OBSCanvasAutoRelease OBSBasic::GetActiveCanvas() const
{
	if (!activeCanvasUuid.empty()) {
		OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(activeCanvasUuid.c_str());
		if (canvas) {
			return canvas;
		}
	}
	return obs_get_main_canvas();
}

bool OBSBasic::GetActiveCanvasVideoInfo(obs_video_info *ovi) const
{
	if (!ovi) {
		return false;
	}
	OBSCanvasAutoRelease canvas = GetActiveCanvas();
	return canvas && obs_canvas_get_video_info(canvas, ovi);
}

void OBSBasic::InitializeCanvasTabs()
{
	if (canvasTabs) {
		return;
	}
	canvasTabs = new QTabBar(ui->previewContainer);
	canvasTabs->setDocumentMode(true);
	canvasTabs->setExpanding(false);
	canvasTabs->setDrawBase(false);
	canvasTabs->setAccessibleName(QTStr("OBSPro.CanvasTabs.AccessibleName"));
	ui->previewTextLayout->insertWidget(0, canvasTabs);

	connect(canvasTabs, &QTabBar::currentChanged, this, [this](int index) {
		if (index < 0) {
			return;
		}
		activeCanvasUuid = canvasTabs->tabData(index).toString().toStdString();
		OBSScene mainScene = GetCurrentSceneSetMainScene();
		if (mainScene) {
			ActivateSceneSet(mainScene);
		}
		obs_video_info info{};
		if (GetActiveCanvasVideoInfo(&info)) {
			ResizePreview(info.base_width, info.base_height);
		}
		ui->preview->update();
	});
	RefreshCanvasTabs();
}

void OBSBasic::RefreshCanvasTabs()
{
	if (!canvasTabs) {
		return;
	}
	QSignalBlocker blocker(canvasTabs);
	const QString previous = QString::fromStdString(activeCanvasUuid);
	while (canvasTabs->count() > 0) {
		canvasTabs->removeTab(canvasTabs->count() - 1);
	}

	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	canvasTabs->addTab(QTStr("OBSPro.Settings.Canvas.Main"));
	canvasTabs->setTabData(0, QString::fromUtf8(obs_canvas_get_uuid(mainCanvas)));
	for (const OBS::Canvas &canvasRef : canvases) {
		obs_canvas_t *canvas = canvasRef;
		if (!canvas || (obs_canvas_get_flags(canvas) & EPHEMERAL)) {
			continue;
		}
		const int index = canvasTabs->addTab(QString::fromUtf8(obs_canvas_get_name(canvas)));
		canvasTabs->setTabData(index, QString::fromUtf8(obs_canvas_get_uuid(canvas)));
	}
	int selected = -1;
	for (int index = 0; index < canvasTabs->count(); ++index) {
		if (canvasTabs->tabData(index).toString() == previous) {
			selected = index;
			break;
		}
	}
	if (selected < 0) {
		selected = 0;
	}
	canvasTabs->setCurrentIndex(selected);
	activeCanvasUuid = canvasTabs->tabData(selected).toString().toStdString();
	canvasTabs->setVisible(canvasTabs->count() > 1);

	OBSScene mainScene = GetCurrentSceneSetMainScene();
	if (mainScene) {
		ActivateSceneSet(mainScene);
	}
	obs_video_info info{};
	if (GetActiveCanvasVideoInfo(&info)) {
		ResizePreview(info.base_width, info.base_height);
	}
	ui->preview->update();
}

OBSScene OBSBasic::FindSceneSetVariant(obs_canvas_t *canvas, const char *sceneSetId) const
{
	if (!canvas || !sceneSetId || !*sceneSetId) {
		return nullptr;
	}
	struct Context {
		const char *id;
		obs_scene_t *scene = nullptr;
	} context{sceneSetId};
	obs_canvas_enum_scenes(
		canvas,
		[](void *data, obs_source_t *source) {
			auto *context = static_cast<Context *>(data);
			OBSDataAutoRelease settings = obs_source_get_private_settings(source);
			if (strcmp(obs_data_get_string(settings, SceneSetIdKey), context->id) != 0) {
				return true;
			}
			context->scene = obs_scene_from_source(source);
			return false;
		},
		&context);
	return OBSScene(context.scene);
}

OBSScene OBSBasic::GetCurrentSceneSetMainScene() const
{
	QListWidgetItem *selected = ui->scenes->currentItem();
	return selected ? GetOBSRef<OBSScene>(selected) : OBSScene();
}

void OBSBasic::SetEditorScene(OBSScene scene)
{
	if (!scene) {
		return;
	}
	activeCanvasSceneSignals.clear();
	currentScene = scene.Get();
	ui->sources->Clear();
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			static_cast<OBSBasic *>(data)->ui->sources->Add(item);
			return true;
		},
		this);

	OBSCanvasAutoRelease sceneCanvas = obs_source_get_canvas(obs_scene_get_source(scene));
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	if (sceneCanvas && sceneCanvas != mainCanvas) {
		signal_handler_t *handler = obs_source_get_signal_handler(obs_scene_get_source(scene));
		activeCanvasSceneSignals.emplace_back(
			std::make_shared<OBSSignal>(handler, "item_add", OBSBasic::SceneItemAdded, this));
		activeCanvasSceneSignals.emplace_back(
			std::make_shared<OBSSignal>(handler, "reorder", OBSBasic::SceneReordered, this));
		activeCanvasSceneSignals.emplace_back(
			std::make_shared<OBSSignal>(handler, "refresh", OBSBasic::SceneRefreshed, this));
	}

	UpdateContextBar(true);
	UpdatePreviewProgramIndicators();
	OnEvent(OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED);
}

void OBSBasic::ActivateSceneSet(OBSScene mainScene)
{
	if (!mainScene) {
		return;
	}
	const char *sceneSetId = obs_source_get_uuid(obs_scene_get_source(mainScene));
	for (const OBS::Canvas &canvasRef : canvases) {
		obs_canvas_t *canvas = canvasRef;
		if (!canvas || (obs_canvas_get_flags(canvas) & EPHEMERAL)) {
			continue;
		}
		OBSScene variant = FindSceneSetVariant(canvas, sceneSetId);
		if (variant) {
			obs_canvas_set_channel(canvas, 0, obs_scene_get_source(variant));
		}
	}

	OBSCanvasAutoRelease activeCanvas = GetActiveCanvas();
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	if (!activeCanvas || activeCanvas == mainCanvas) {
		SetEditorScene(mainScene);
		return;
	}
	OBSScene variant = FindSceneSetVariant(activeCanvas, sceneSetId);
	SetEditorScene(variant ? variant : mainScene);
}

void OBSBasic::InitializeCanvasSceneSets(obs_canvas_t *canvas, bool duplicateLayout, bool independentSources)
{
	if (!canvas) {
		return;
	}
	std::vector<OBSScene> mainScenes;
	std::unordered_set<std::string> createdSceneSets;
	obs_enum_scenes(
		[](void *data, obs_source_t *source) {
			static_cast<std::vector<OBSScene> *>(data)->emplace_back(obs_scene_from_source(source));
			return true;
		},
		&mainScenes);

	// Create all variants first so nested scene references can be resolved in
	// the second pass without depending on scene order.
	for (const OBSScene &mainScene : mainScenes) {
		obs_source_t *mainSource = obs_scene_get_source(mainScene);
		const char *sceneSetId = obs_source_get_uuid(mainSource);
		if (FindSceneSetVariant(canvas, sceneSetId)) {
			continue;
		}
		OBSSceneAutoRelease existing = obs_canvas_get_scene_by_name(canvas, obs_source_get_name(mainSource));
		if (existing) {
			OBSDataAutoRelease settings = obs_source_get_private_settings(obs_scene_get_source(existing));
			if (!*obs_data_get_string(settings, SceneSetIdKey)) {
				SetSceneSetMetadata(existing, sceneSetId, obs_source_get_name(mainSource));
				continue;
			}
		}
		OBSSceneAutoRelease variant = obs_canvas_scene_create(canvas, obs_source_get_name(mainSource));
		SetSceneSetMetadata(variant, sceneSetId, obs_source_get_name(mainSource));
		createdSceneSets.emplace(sceneSetId);
	}

	if (duplicateLayout) {
		std::unordered_map<std::string, OBSSource> independentCopies;
		using CopyScene = std::function<void(obs_scene_t *, obs_scene_t *)>;
		CopyScene copyScene;
		copyScene = [this, canvas, independentSources, &independentCopies,
			     &copyScene](obs_scene_t *sourceScene, obs_scene_t *targetScene) {
			struct CopyContext {
				OBSBasic *main;
				obs_canvas_t *canvas;
				obs_scene_t *target;
				bool independent;
				std::unordered_map<std::string, OBSSource> *copies;
				CopyScene *copyScene;
			} context{this, canvas, targetScene, independentSources, &independentCopies, &copyScene};

			obs_scene_enum_items(
				sourceScene,
				[](obs_scene_t *, obs_sceneitem_t *sourceItem, void *data) {
					auto *context = static_cast<CopyContext *>(data);
					obs_source_t *source = obs_sceneitem_get_source(sourceItem);
					obs_source_t *targetSource = source;
					obs_sceneitem_t *targetItem = nullptr;
					OBSSourceAutoRelease temporary;

					if (obs_source_is_group(source)) {
						const std::string uuid = obs_source_get_uuid(source);
						auto found = context->copies->find(uuid);
						if (found == context->copies->end()) {
							const std::string name =
								UniqueSourceName(obs_source_get_name(source),
										 obs_canvas_get_name(context->canvas));
							targetItem = obs_scene_add_group(context->target, name.c_str());
							if (!targetItem) {
								return true;
							}
							targetSource = obs_sceneitem_get_source(targetItem);
							context->copies->emplace(uuid, OBSSource(targetSource));

							obs_source_copy_filters(targetSource, source);
							OBSDataAutoRelease sourceSettings =
								obs_source_get_private_settings(source);
							OBSDataAutoRelease targetSettings =
								obs_source_get_private_settings(targetSource);
							obs_data_apply(targetSettings, sourceSettings);
							(*context->copyScene)(obs_group_from_source(source),
									      obs_group_from_source(targetSource));
						} else {
							targetSource = found->second;
						}
					} else if (obs_source_is_scene(source)) {
						OBSScene linked = context->main->FindSceneSetVariant(
							context->canvas, obs_source_get_uuid(source));
						if (linked) {
							targetSource = obs_scene_get_source(linked);
						}
					} else if (context->independent) {
						const std::string uuid = obs_source_get_uuid(source);
						auto found = context->copies->find(uuid);
						if (found == context->copies->end()) {
							const std::string name =
								UniqueSourceName(obs_source_get_name(source),
										 obs_canvas_get_name(context->canvas));
							temporary = obs_source_duplicate(source, name.c_str(), false);
							if (temporary) {
								found = context->copies
										->emplace(uuid,
											  OBSSource(temporary.Get()))
										.first;
							}
						}
						if (found != context->copies->end()) {
							targetSource = found->second;
						}
					}

					if (!targetItem) {
						targetItem = obs_scene_add(context->target, targetSource);
					}
					if (targetItem) {
						CopySceneItemState(sourceItem, targetItem);
					}
					return true;
				},
				&context);
		};

		for (const OBSScene &mainScene : mainScenes) {
			obs_source_t *mainSource = obs_scene_get_source(mainScene);
			if (createdSceneSets.find(obs_source_get_uuid(mainSource)) == createdSceneSets.end()) {
				continue;
			}
			OBSScene target = FindSceneSetVariant(canvas, obs_source_get_uuid(mainSource));
			if (!target) {
				continue;
			}
			copyScene(mainScene, target);
		}
	}

	OBSScene selected = GetCurrentSceneSetMainScene();
	if (!selected && !mainScenes.empty()) {
		selected = mainScenes.front();
	}
	if (selected) {
		OBSScene variant = FindSceneSetVariant(canvas, obs_source_get_uuid(obs_scene_get_source(selected)));
		if (variant) {
			obs_canvas_set_channel(canvas, 0, obs_scene_get_source(variant));
		}
	}
	SaveProject();
}

void OBSBasic::RemoveSceneSetVariants(OBSScene mainScene)
{
	if (!mainScene) {
		return;
	}
	const char *sceneSetId = obs_source_get_uuid(obs_scene_get_source(mainScene));
	for (const OBS::Canvas &canvasRef : canvases) {
		OBSScene variant = FindSceneSetVariant(canvasRef, sceneSetId);
		if (variant) {
			obs_canvas_scene_remove(variant);
		}
	}
}

void OBSBasic::RenameSceneSetVariants(OBSScene mainScene, const char *name)
{
	if (!mainScene || !name || !*name) {
		return;
	}
	const char *sceneSetId = obs_source_get_uuid(obs_scene_get_source(mainScene));
	for (const OBS::Canvas &canvasRef : canvases) {
		OBSScene variant = FindSceneSetVariant(canvasRef, sceneSetId);
		if (!variant) {
			continue;
		}
		obs_source_set_name(obs_scene_get_source(variant), name);
		SetSceneSetMetadata(variant, sceneSetId, name);
	}
	RefreshCanvasTabs();
}
