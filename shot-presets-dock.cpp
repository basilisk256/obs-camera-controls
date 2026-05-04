#include "shot-presets-dock.hpp"
#include "shot-presets-shared.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QGroupBox>
#include <QSignalMapper>
#include <QFormLayout>
#include <QGridLayout>
#include <QMenu>
#include <QAction>
#include <QByteArray>
#include <QLineEdit>
#include <QMainWindow>
#include <QDockWidget>
#include <QTimer>
#include <QPointer>
#include <QAction>

/* ── Dock Widget ────────────────────────────────────────────── */

ShotPresetsDock::ShotPresetsDock(QWidget *parent)
	: QWidget(parent),
	  mainLayout(nullptr),
	  durationSpin(nullptr),
	  easingTypeCb(nullptr),
	  easingFuncCb(nullptr),
	  presetsContainer(nullptr),
	  presetsLayout(nullptr),
	  refreshTimer(nullptr),
	  emptyLabel(nullptr),
	  addBtn(nullptr)
{
	buildUI();

	refreshTimer = new QTimer(this);
	connect(refreshTimer, &QTimer::timeout, this, &ShotPresetsDock::refreshUI);
	refreshTimer->start(1000);
}

void ShotPresetsDock::buildUI()
{
	mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(6, 6, 6, 6);
	mainLayout->setSpacing(4);

	/* Duration + ATEM-sync delay controls share one row to keep the
	 * dock compact. ATEM sync delays the framing change after firing
	 * the ATEM switch so both land on the same video frame in OBS. */
	QHBoxLayout *durRow = new QHBoxLayout();
	durRow->addWidget(new QLabel("Duration:"));
	durationSpin = new QSpinBox();
	durationSpin->setRange(50, 5000);
	durationSpin->setSuffix(" ms");
	durationSpin->setSingleStep(50);
	durationSpin->setValue(shot_presets_get_duration());
	connect(durationSpin, QOverload<int>::of(&QSpinBox::valueChanged),
		this, &ShotPresetsDock::onDurationChanged);
	durRow->addWidget(durationSpin);

	durRow->addSpacing(8);
	durRow->addWidget(new QLabel("Fade:"));
	fadeDurationSpin = new QSpinBox();
	fadeDurationSpin->setRange(50, 2000);
	fadeDurationSpin->setSuffix(" ms");
	fadeDurationSpin->setSingleStep(50);
	fadeDurationSpin->setToolTip(
		"Default fade duration (per-row Fade button + ATEM hardware "
		"mix transition rate). Per-preset Duration in Edit panel "
		"overrides this when set.");
	fadeDurationSpin->setValue(shot_presets_get_fade_duration());
	connect(fadeDurationSpin, QOverload<int>::of(&QSpinBox::valueChanged),
		this, &ShotPresetsDock::onFadeDurationChanged);
	durRow->addWidget(fadeDurationSpin);

	durRow->addSpacing(8);
	durRow->addWidget(new QLabel("ATEM sync:"));
	atemSyncSpin = new QSpinBox();
	atemSyncSpin->setRange(0, 1000);
	atemSyncSpin->setSuffix(" ms");
	atemSyncSpin->setSingleStep(10);
	atemSyncSpin->setSpecialValueText("(off)");
	atemSyncSpin->setToolTip(
		"Delay the framing change by this many ms after firing the "
		"ATEM input switch so both land together in OBS. Tune to "
		"USB capture latency + Render Delay filter time. 0 = off.");
	atemSyncSpin->setValue(shot_presets_get_atem_sync_delay());
	connect(atemSyncSpin, QOverload<int>::of(&QSpinBox::valueChanged),
		this, &ShotPresetsDock::onAtemSyncDelayChanged);
	durRow->addWidget(atemSyncSpin);

	durRow->addSpacing(8);
	durRow->addWidget(new QLabel("Fade sync:"));
	fadeSyncSpin = new QSpinBox();
	fadeSyncSpin->setRange(0, 1000);
	fadeSyncSpin->setSuffix(" ms");
	fadeSyncSpin->setSingleStep(10);
	fadeSyncSpin->setSpecialValueText("(off)");
	fadeSyncSpin->setToolTip(
		"Same as ATEM sync but applied to FADE-mode triggers only. "
		"For fades, the framing crossfade should *start* when the "
		"ATEM crossfade *start* is visible in OBS — typically lower "
		"than the cut sync value.");
	fadeSyncSpin->setValue(shot_presets_get_fade_sync_delay());
	connect(fadeSyncSpin, QOverload<int>::of(&QSpinBox::valueChanged),
		this, &ShotPresetsDock::onFadeSyncDelayChanged);
	durRow->addWidget(fadeSyncSpin);
	mainLayout->addLayout(durRow);

	/* Empty-state label shown when no Shot Presets filter exists on
	 * any source in the current scene. */
	emptyLabel = new QLabel(
		"No Shot Presets filter on this scene.\n"
		"Right-click a source \u2192 Filters \u2192 + \u2192 Shot Presets.");
	emptyLabel->setAlignment(Qt::AlignCenter);
	emptyLabel->setWordWrap(true);
	emptyLabel->setStyleSheet(
		"QLabel { color: #888; padding: 12px; font-style: italic; }");
	emptyLabel->setVisible(false);
	mainLayout->addWidget(emptyLabel);

	/* Presets area */
	presetsContainer = new QWidget();
	presetsLayout = new QVBoxLayout(presetsContainer);
	presetsLayout->setContentsMargins(0, 0, 0, 0);
	presetsLayout->setSpacing(2);
	mainLayout->addWidget(presetsContainer);

	/* Add-preset button pinned at the bottom of the list */
	addBtn = new QPushButton("+ Add Shot");
	addBtn->setMinimumHeight(32);
	addBtn->setStyleSheet(
		"QPushButton { font-size: 12px; padding: 6px; }");
	connect(addBtn, &QPushButton::clicked, this,
		&ShotPresetsDock::onAddPreset);
	mainLayout->addWidget(addBtn);

	mainLayout->addStretch();

	refreshUI();
}

void ShotPresetsDock::refreshUI()
{
	bool hasActive = shot_presets_has_active() != 0;
	int count = hasActive ? shot_presets_get_count() : 0;

	/* Toggle empty-state vs normal UI based on whether the current
	 * scene has a Shot Presets filter. */
	if (emptyLabel)
		emptyLabel->setVisible(!hasActive);
	if (durationSpin)
		durationSpin->setEnabled(hasActive);
	if (addBtn)
		addBtn->setEnabled(hasActive);
	if (presetsContainer)
		presetsContainer->setVisible(hasActive);

	/* Update duration from filter if it changed */
	int dur = shot_presets_get_duration();
	if (durationSpin->value() != dur)
		durationSpin->setValue(dur);

	/* Sync the ATEM-sync delay spinner without stealing focus */
	if (atemSyncSpin && !atemSyncSpin->hasFocus()) {
		int sync = shot_presets_get_atem_sync_delay();
		if (atemSyncSpin->value() != sync) {
			QSignalBlocker b(atemSyncSpin);
			atemSyncSpin->setValue(sync);
		}
	}

	if (fadeDurationSpin && !fadeDurationSpin->hasFocus()) {
		int fdur = shot_presets_get_fade_duration();
		if (fadeDurationSpin->value() != fdur) {
			QSignalBlocker b(fadeDurationSpin);
			fadeDurationSpin->setValue(fdur);
		}
	}

	if (fadeSyncSpin && !fadeSyncSpin->hasFocus()) {
		int fsync = shot_presets_get_fade_sync_delay();
		if (fadeSyncSpin->value() != fsync) {
			QSignalBlocker b(fadeSyncSpin);
			fadeSyncSpin->setValue(fsync);
		}
	}

	/* Rebuild preset buttons if count changed */
	if (count != presetRows.size()) {
		/* Clear old rows */
		QLayoutItem *item;
		while ((item = presetsLayout->takeAt(0)) != nullptr) {
			if (item->widget())
				item->widget()->deleteLater();
			delete item;
		}
		presetRows.clear();

		for (int i = 0; i < count; i++) {
			QWidget *rowContainer = new QWidget();
			QVBoxLayout *rowContainerLayout = new QVBoxLayout(rowContainer);
			rowContainerLayout->setContentsMargins(0, 0, 0, 0);
			rowContainerLayout->setSpacing(2);

			QHBoxLayout *row = new QHBoxLayout();
			row->setSpacing(4);

			QPushButton *goBtn = new QPushButton();
			goBtn->setSizePolicy(QSizePolicy::Expanding,
					     QSizePolicy::Preferred);
			goBtn->setMinimumHeight(40);
			goBtn->setToolTip("Animate to this preset (uses Duration)");
			goBtn->setStyleSheet(
				"QPushButton { font-size: 14px; font-weight: bold; padding: 6px 12px; }"
				"QPushButton:disabled { color: #666; }");
			connect(goBtn, &QPushButton::clicked, this,
				[this, i]() { onPresetClicked(i); });
			goBtn->setContextMenuPolicy(Qt::NoContextMenu);
			row->addWidget(goBtn);

			/* Fade button forces a cross-dissolve regardless of the
			 * preset's configured transition. Same width as Cut. */
			QPushButton *fadeBtn = new QPushButton("Fade");
			fadeBtn->setToolTip("Fade (cross-dissolve) to this preset");
			fadeBtn->setFixedWidth(50);
			fadeBtn->setMinimumHeight(40);
			fadeBtn->setStyleSheet(
				"QPushButton { font-size: 12px; font-weight: bold; padding: 4px; background: #2a3a5a; }"
				"QPushButton:hover { background: #3a4a7a; }"
				"QPushButton:disabled { color: #666; background: #2a2a2a; }");
			connect(fadeBtn, &QPushButton::clicked, this,
				[this, i]() { onFadeClicked(i); });
			row->addWidget(fadeBtn);

			QPushButton *cutBtn = new QPushButton("Cut");
			cutBtn->setToolTip("Cut to this preset instantly (no animation)");
			cutBtn->setFixedWidth(50);
			cutBtn->setMinimumHeight(40);
			cutBtn->setStyleSheet(
				"QPushButton { font-size: 12px; font-weight: bold; padding: 4px; background: #5a2a2a; }"
				"QPushButton:hover { background: #7a3a3a; }"
				"QPushButton:disabled { color: #666; background: #2a2a2a; }");
			connect(cutBtn, &QPushButton::clicked, this,
				[this, i]() { onCutClicked(i); });
			row->addWidget(cutBtn);

			QPushButton *capBtn = new QPushButton("Save");
			capBtn->setToolTip("Save current transform to this preset");
			capBtn->setFixedWidth(60);
			capBtn->setMinimumHeight(40);
			capBtn->setStyleSheet("QPushButton { font-size: 11px; padding: 4px; }");
			connect(capBtn, &QPushButton::clicked, this,
				[this, i]() { onCaptureClicked(i); });
			row->addWidget(capBtn);

			QPushButton *editBtn = new QPushButton("Edit");
			editBtn->setToolTip("Edit crop & per-preset duration");
			editBtn->setFixedWidth(50);
			editBtn->setMinimumHeight(40);
			editBtn->setCheckable(true);
			editBtn->setStyleSheet("QPushButton { font-size: 11px; padding: 4px; }");
			connect(editBtn, &QPushButton::clicked, this,
				[this, i]() { onEditToggled(i); });
			row->addWidget(editBtn);

			/* Per-scene default toggle. Filled star = this preset is
			 * the scene's default framing (snapped to on activation).
			 * Click toggles; only one default per scene at a time. */
			QPushButton *defBtn = new QPushButton(QString::fromUtf8("☆"));
			defBtn->setToolTip(
				"Set as scene default (snapped to on scene activation)");
			defBtn->setFixedWidth(34);
			defBtn->setMinimumHeight(40);
			defBtn->setCheckable(true);
			defBtn->setStyleSheet(
				"QPushButton { font-size: 16px; padding: 2px; color: #888; }"
				"QPushButton:checked { color: #ffd54a; background: #3a2f10; }"
				"QPushButton:hover { color: #fff; }");
			connect(defBtn, &QPushButton::clicked, this,
				[this, i]() { onDefaultToggled(i); });
			row->addWidget(defBtn);

			QWidget *rowWidget = new QWidget();
			rowWidget->setLayout(row);
			rowContainerLayout->addWidget(rowWidget);

			/* Edit panel (hidden by default) */
			QWidget *editPanel = new QWidget();
			editPanel->setVisible(false);
			editPanel->setStyleSheet(
				"QWidget { background: #202020; border-radius: 4px; }"
				"QLabel { background: transparent; }"
				"QSpinBox { background: #2a2a2a; }");
			QVBoxLayout *epLayout = new QVBoxLayout(editPanel);
			epLayout->setContentsMargins(8, 6, 8, 6);
			epLayout->setSpacing(4);

			QHBoxLayout *nameRow = new QHBoxLayout();
			nameRow->addWidget(new QLabel("Name:"));
			QLineEdit *nameEd = new QLineEdit();
			nameEd->setToolTip("Rename this shot");
			connect(nameEd, &QLineEdit::editingFinished, this,
				[this, i, nameEd]() {
					onNameChanged(i, nameEd->text());
				});
			nameRow->addWidget(nameEd);
			epLayout->addLayout(nameRow);

			QHBoxLayout *transRow = new QHBoxLayout();
			transRow->addWidget(new QLabel("Transition:"));
			QComboBox *transCb = new QComboBox();
			transCb->addItem("Move (animated)", 0);
			transCb->addItem("Cut (instant)", 1);
			transCb->addItem("Fade", 2);
			transCb->setToolTip(
				"How the main preset button transitions to this shot");
			connect(transCb,
				QOverload<int>::of(&QComboBox::currentIndexChanged),
				this, [this, i, transCb](int) {
					int t = transCb->currentData().toInt();
					onTransitionChanged(i, t);
				});
			transRow->addWidget(transCb);
			transRow->addStretch();
			epLayout->addLayout(transRow);

			QHBoxLayout *durRow2 = new QHBoxLayout();
			durRow2->addWidget(new QLabel("Duration:"));
			QSpinBox *pDur = new QSpinBox();
			pDur->setRange(0, 5000);
			pDur->setSuffix(" ms");
			pDur->setSingleStep(50);
			pDur->setSpecialValueText("(use default)");
			pDur->setToolTip("Per-preset duration. 0 = use the default above.");
			connect(pDur, QOverload<int>::of(&QSpinBox::valueChanged),
				this, [this, i](int v) {
					onPresetDurationChanged(i, v);
				});
			durRow2->addWidget(pDur);
			durRow2->addStretch();
			epLayout->addLayout(durRow2);

			/* ATEM program input. Fires HTTP POST to FNN runtime
			 * on any preset trigger so the ATEM Mini cuts to the
			 * matching camera in lockstep with the framing change.
			 * 0 = no ATEM action for this preset. */
			QHBoxLayout *atemRow = new QHBoxLayout();
			atemRow->addWidget(new QLabel("ATEM input:"));
			QSpinBox *atemSpin = new QSpinBox();
			atemSpin->setRange(0, 8);
			atemSpin->setSpecialValueText("(none)");
			atemSpin->setToolTip(
				"ATEM program input to switch to when this shot fires. "
				"0 = no ATEM action. Requires FNN runtime running on "
				"127.0.0.1:4173.");
			connect(atemSpin, QOverload<int>::of(&QSpinBox::valueChanged),
				this, [this, i](int v) {
					onAtemInputChanged(i, v);
				});
			atemRow->addWidget(atemSpin);
			atemRow->addStretch();
			epLayout->addLayout(atemRow);

			/* Edit panel intentionally minimal \u2014 framing is set by
			 * dragging in OBS preview, then committed via the row's
			 * Save button. The transform/crop/bounds spinboxes used
			 * to live here but bloated the panel; backend still
			 * stores all of those, just not editable from the dock. */
			QPushButton *removeBtn = new QPushButton("Remove this shot");
			removeBtn->setStyleSheet(
				"QPushButton { font-size: 11px; padding: 4px 8px; color: #e88; }"
				"QPushButton:hover { color: #fff; background: #7a2a2a; }");
			connect(removeBtn, &QPushButton::clicked, this,
				[this, i]() { onRemovePreset(i); });
			epLayout->addWidget(removeBtn);

			rowContainerLayout->addWidget(editPanel);
			presetsLayout->addWidget(rowContainer);

			PresetRow pr = {goBtn, fadeBtn, cutBtn, capBtn, editBtn,
					defBtn, editPanel, pDur, transCb, nameEd,
					atemSpin, removeBtn};
			presetRows.append(pr);
		}
	}

	/* Update button labels & spinbox values */
	int defaultIdx = shot_presets_get_default_preset();
	int currentIdx = shot_presets_get_current_preset();
	const QString goBtnBase =
		"QPushButton { font-size: 14px; font-weight: bold; padding: 6px 12px; }"
		"QPushButton:disabled { color: #666; }";
	const QString goBtnSelected =
		"QPushButton { font-size: 14px; font-weight: bold; padding: 6px 12px; "
		"background: #2c5fa6; color: #fff; "
		"border: 1px solid #5a8fd6; }"
		"QPushButton:disabled { color: #888; }";
	for (int i = 0; i < count && i < presetRows.size(); i++) {
		const char *name = shot_presets_get_name(i);
		bool active = shot_presets_is_active(i);
		QString label = QString("%1%2")
			.arg(active ? "" : "(empty) ")
			.arg(name && name[0] ? name : "Untitled");
		presetRows[i].goBtn->setText(label);
		presetRows[i].goBtn->setEnabled(active);
		presetRows[i].goBtn->setStyleSheet(
			i == currentIdx ? goBtnSelected : goBtnBase);
		presetRows[i].cutBtn->setEnabled(active);
		if (presetRows[i].fadeBtn)
			presetRows[i].fadeBtn->setEnabled(active);

		/* Sync star button to backend default state */
		QPushButton *db = presetRows[i].defaultBtn;
		if (db) {
			bool isDefault = (i == defaultIdx);
			if (db->isChecked() != isDefault) {
				QSignalBlocker blk(db);
				db->setChecked(isDefault);
			}
			db->setText(QString::fromUtf8(isDefault ? "★" : "☆"));
		}

		int pDur = shot_presets_get_preset_duration(i);
		QSpinBox *pd = presetRows[i].presetDurSpin;
		if (pd && !pd->hasFocus() && pd->value() != pDur) {
			QSignalBlocker b(pd);
			pd->setValue(pDur);
		}

		int tt = shot_presets_get_transition(i);
		QComboBox *tc = presetRows[i].transitionCb;
		if (tc) {
			int idx = tc->findData(tt);
			if (idx < 0) idx = 0;
			if (tc->currentIndex() != idx) {
				QSignalBlocker b(tc);
				tc->setCurrentIndex(idx);
			}
		}

		QLineEdit *ne = presetRows[i].nameEdit;
		if (ne && !ne->hasFocus()) {
			QString current = ne->text();
			QString actual = QString::fromUtf8(name ? name : "");
			if (current != actual) {
				QSignalBlocker b(ne);
				ne->setText(actual);
			}
		}

		int atemInput = shot_presets_get_atem_input(i);
		QSpinBox *as = presetRows[i].atemInputSpin;
		if (as && !as->hasFocus() && as->value() != atemInput) {
			QSignalBlocker b(as);
			as->setValue(atemInput);
		}
	}
}

void ShotPresetsDock::onPresetClicked(int index)
{
	if (shot_presets_get_transition(index) == SHOT_TRANSITION_CUT)
		shot_presets_cut(index);
	else
		shot_presets_go_to(index);
}

void ShotPresetsDock::onCutClicked(int index)
{
	shot_presets_cut(index);
}

void ShotPresetsDock::onFadeClicked(int index)
{
	shot_presets_fade(index);
}

void ShotPresetsDock::onEditToggled(int index)
{
	if (index < 0 || index >= presetRows.size())
		return;
	QWidget *panel = presetRows[index].editPanel;
	QPushButton *btn = presetRows[index].editBtn;
	if (panel)
		panel->setVisible(btn && btn->isChecked());
}

void ShotPresetsDock::onPresetDurationChanged(int index, int value)
{
	shot_presets_set_preset_duration(index, value);
}

void ShotPresetsDock::onTransitionChanged(int index, int type)
{
	shot_presets_set_transition(index, type);
}

void ShotPresetsDock::onAtemInputChanged(int index, int value)
{
	shot_presets_set_atem_input(index, value);
}

void ShotPresetsDock::onNameChanged(int index, const QString &name)
{
	QByteArray utf = name.toUtf8();
	shot_presets_set_name(index, utf.constData());
}

void ShotPresetsDock::onAddPreset()
{
	shot_presets_add_preset(nullptr);
	refreshUI();
}

void ShotPresetsDock::onRemovePreset(int index)
{
	shot_presets_remove_preset(index);
	refreshUI();
}

void ShotPresetsDock::onDefaultToggled(int index)
{
	/* Toggle: if this preset is already the default, clear it; otherwise
	 * set it as the default. The backend stores at most one default per
	 * scene, so setting overwrites any previous one. */
	int current = shot_presets_get_default_preset();
	shot_presets_set_default_preset(current == index ? -1 : index);
	refreshUI();
}

void ShotPresetsDock::onCaptureClicked(int index)
{
	int ok = shot_presets_capture(index);
	refreshUI();

	/* Visual confirmation: green flash on success (data committed to
	 * the active bucket); red flash on failure (capture didn't happen
	 * — usually means source missing from the active scene or filter
	 * restricted via properties). Tooltip on failure points the user
	 * at the OBS log for the reason. */
	if (index < 0 || index >= presetRows.size())
		return;
	QPushButton *btn = presetRows[index].captureBtn;
	if (!btn)
		return;
	const QString restoreStyle =
		"QPushButton { font-size: 11px; padding: 4px; }";
	const QString flashStyle = ok
		? QString("QPushButton { font-size: 11px; padding: 4px; "
		          "background: #2e8b3a; color: #fff; "
		          "border: 1px solid #4cc25f; }")
		: QString("QPushButton { font-size: 11px; padding: 4px; "
		          "background: #8b2e2e; color: #fff; "
		          "border: 1px solid #c25a4c; }");
	btn->setStyleSheet(flashStyle);
	const QString restoreTip = "Save current transform to this preset";
	if (!ok) {
		btn->setToolTip(
			"Capture failed — source not in active scene or filter "
			"restricted via properties dialog. See OBS log for details.");
	}
	QPointer<QPushButton> guard(btn);
	QTimer::singleShot(900, this, [guard, restoreStyle, restoreTip]() {
		if (guard) {
			guard->setStyleSheet(restoreStyle);
			guard->setToolTip(restoreTip);
		}
	});
}

void ShotPresetsDock::onDurationChanged(int value)
{
	shot_presets_set_duration(value);
}

void ShotPresetsDock::onAtemSyncDelayChanged(int value)
{
	shot_presets_set_atem_sync_delay(value);
}

void ShotPresetsDock::onFadeDurationChanged(int value)
{
	shot_presets_set_fade_duration(value);
}

void ShotPresetsDock::onFadeSyncDelayChanged(int value)
{
	shot_presets_set_fade_sync_delay(value);
}

/* ── Registration ───────────────────────────────────────────── */

static void place_shot_presets_dock()
{
	QMainWindow *main = (QMainWindow *)obs_frontend_get_main_window();
	if (!main)
		return;

	QDockWidget *ours = nullptr;
	QDockWidget *controls = nullptr;
	QDockWidget *transitions = nullptr;
	for (QDockWidget *d : main->findChildren<QDockWidget *>()) {
		const QString name = d->objectName();
		if (name == "shot-presets-dock" ||
		    d->windowTitle() == QStringLiteral("Shot Presets"))
			ours = d;
		else if (name == "controlsDock")
			controls = d;
		else if (name == "transitionsDock")
			transitions = d;
	}
	if (!ours)
		return;

	/* If floating or hidden, force-dock it to the bottom row
	 * next to Controls and show it. */
	if (ours->isFloating() || !ours->isVisible()) {
		QDockWidget *anchor = controls ? controls : transitions;
		if (anchor)
			main->splitDockWidget(anchor, ours, Qt::Horizontal);
		else
			main->addDockWidget(Qt::BottomDockWidgetArea, ours);
		ours->setFloating(false);
		ours->show();
		ours->raise();
		/* Keep the Docks-menu checkbox in sync. */
		QAction *act = ours->toggleViewAction();
		if (act && !act->isChecked())
			act->setChecked(true);
	}
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;
	/* Defer placement so we run after any late restoreState() calls. */
	QTimer::singleShot(0, place_shot_presets_dock);
	QTimer::singleShot(250, place_shot_presets_dock);
	QTimer::singleShot(1000, place_shot_presets_dock);
}

extern "C" void shot_presets_dock_init(void)
{
	obs_frontend_add_dock_by_id("shot-presets-dock", "Shot Presets",
				    new ShotPresetsDock());
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
}
