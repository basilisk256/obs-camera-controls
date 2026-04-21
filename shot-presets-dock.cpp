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

	/* Duration control */
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

			/* Position / Scale / Rotation / Alignment.
			 * All live-apply via shot_presets_set_transform
			 * (same feel as the Crop fields below). */
			QGridLayout *xformGrid = new QGridLayout();
			xformGrid->setSpacing(4);

			xformGrid->addWidget(new QLabel("Pos X:"), 0, 0);
			QDoubleSpinBox *posX = new QDoubleSpinBox();
			posX->setRange(-10000, 10000);
			posX->setDecimals(1);
			posX->setSingleStep(1.0);
			xformGrid->addWidget(posX, 0, 1);
			xformGrid->addWidget(new QLabel("Y:"), 0, 2);
			QDoubleSpinBox *posY = new QDoubleSpinBox();
			posY->setRange(-10000, 10000);
			posY->setDecimals(1);
			posY->setSingleStep(1.0);
			xformGrid->addWidget(posY, 0, 3);

			xformGrid->addWidget(new QLabel("Scale X:"), 1, 0);
			QDoubleSpinBox *scX = new QDoubleSpinBox();
			scX->setRange(0.001, 100.0);
			scX->setDecimals(3);
			scX->setSingleStep(0.01);
			xformGrid->addWidget(scX, 1, 1);
			xformGrid->addWidget(new QLabel("Y:"), 1, 2);
			QDoubleSpinBox *scY = new QDoubleSpinBox();
			scY->setRange(0.001, 100.0);
			scY->setDecimals(3);
			scY->setSingleStep(0.01);
			xformGrid->addWidget(scY, 1, 3);

			xformGrid->addWidget(new QLabel("Rotation:"), 2, 0);
			QDoubleSpinBox *rot = new QDoubleSpinBox();
			rot->setRange(-360.0, 360.0);
			rot->setDecimals(2);
			rot->setSuffix(" \u00B0");
			rot->setSingleStep(1.0);
			xformGrid->addWidget(rot, 2, 1);
			xformGrid->addWidget(new QLabel("Align:"), 2, 2);
			QComboBox *alignCb = new QComboBox();
			alignCb->addItem("Top Left",   5);
			alignCb->addItem("Top",        4);
			alignCb->addItem("Top Right",  6);
			alignCb->addItem("Left",       1);
			alignCb->addItem("Center",     0);
			alignCb->addItem("Right",      2);
			alignCb->addItem("Bot Left",   9);
			alignCb->addItem("Bottom",     8);
			alignCb->addItem("Bot Right", 10);
			xformGrid->addWidget(alignCb, 2, 3);
			epLayout->addLayout(xformGrid);

			QGridLayout *boundsGrid = new QGridLayout();
			boundsGrid->setSpacing(4);
			boundsGrid->addWidget(new QLabel("Bounds:"), 0, 0);
			QComboBox *btCb = new QComboBox();
			btCb->addItem("No bounds",          0);
			btCb->addItem("Stretch",            1);
			btCb->addItem("Fit (scale inner)",  2);
			btCb->addItem("Fill (scale outer)", 3);
			btCb->addItem("Scale to width",     4);
			btCb->addItem("Scale to height",    5);
			btCb->addItem("Max only",           6);
			boundsGrid->addWidget(btCb, 0, 1, 1, 3);

			boundsGrid->addWidget(new QLabel("W:"), 1, 0);
			QDoubleSpinBox *bW = new QDoubleSpinBox();
			bW->setRange(0.0, 10000.0);
			bW->setDecimals(1);
			bW->setSingleStep(1.0);
			boundsGrid->addWidget(bW, 1, 1);
			boundsGrid->addWidget(new QLabel("H:"), 1, 2);
			QDoubleSpinBox *bH = new QDoubleSpinBox();
			bH->setRange(0.0, 10000.0);
			bH->setDecimals(1);
			bH->setSingleStep(1.0);
			boundsGrid->addWidget(bH, 1, 3);

			boundsGrid->addWidget(new QLabel("B.Align:"), 2, 0);
			QComboBox *bAlignCb = new QComboBox();
			bAlignCb->addItem("Top Left",   5);
			bAlignCb->addItem("Top",        4);
			bAlignCb->addItem("Top Right",  6);
			bAlignCb->addItem("Left",       1);
			bAlignCb->addItem("Center",     0);
			bAlignCb->addItem("Right",      2);
			bAlignCb->addItem("Bot Left",   9);
			bAlignCb->addItem("Bottom",     8);
			bAlignCb->addItem("Bot Right", 10);
			boundsGrid->addWidget(bAlignCb, 2, 1, 1, 3);
			epLayout->addLayout(boundsGrid);

			auto xformChanged = [this, i]() { onTransformChanged(i); };
			connect(posX, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				this, xformChanged);
			connect(posY, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				this, xformChanged);
			connect(scX, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				this, xformChanged);
			connect(scY, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				this, xformChanged);
			connect(rot, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				this, xformChanged);
			connect(alignCb,
				QOverload<int>::of(&QComboBox::currentIndexChanged),
				this, [this, i](int) { onTransformChanged(i); });
			connect(btCb,
				QOverload<int>::of(&QComboBox::currentIndexChanged),
				this, [this, i](int) { onTransformChanged(i); });
			connect(bW, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				this, xformChanged);
			connect(bH, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				this, xformChanged);
			connect(bAlignCb,
				QOverload<int>::of(&QComboBox::currentIndexChanged),
				this, [this, i](int) { onTransformChanged(i); });

			QPushButton *pasteBtn = new QPushButton("Paste from scene \u25BE");
			pasteBtn->setToolTip(
				"Copy this source's transform from another scene into this preset");
			pasteBtn->setStyleSheet("QPushButton { font-size: 11px; padding: 4px 8px; }");
			connect(pasteBtn, &QPushButton::clicked, this,
				[this, i, pasteBtn]() {
					QMenu menu;
					struct Ctx { QMenu *m; } ctx{&menu};
					shot_presets_for_each_source_scene(
						[](const char *name, void *u) {
							auto *c = (Ctx *)u;
							c->m->addAction(
								QString::fromUtf8(name));
						}, &ctx);
					if (menu.isEmpty()) {
						menu.addAction("(no other scenes contain this source)")
							->setEnabled(false);
					}
					QAction *chosen = menu.exec(pasteBtn->mapToGlobal(
						QPoint(0, pasteBtn->height())));
					if (chosen && chosen->isEnabled()) {
						QByteArray name = chosen->text().toUtf8();
						shot_presets_paste_from_scene(i, name.constData());
						refreshUI();
					}
				});
			epLayout->addWidget(pasteBtn);

			QGridLayout *cropGrid = new QGridLayout();
			cropGrid->setSpacing(4);
			cropGrid->addWidget(new QLabel("Crop L:"), 0, 0);
			QSpinBox *cL = new QSpinBox();
			cL->setRange(0, 8192);
			cL->setSingleStep(2);
			cropGrid->addWidget(cL, 0, 1);
			cropGrid->addWidget(new QLabel("T:"), 0, 2);
			QSpinBox *cT = new QSpinBox();
			cT->setRange(0, 8192);
			cT->setSingleStep(2);
			cropGrid->addWidget(cT, 0, 3);
			cropGrid->addWidget(new QLabel("R:"), 1, 0);
			QSpinBox *cR = new QSpinBox();
			cR->setRange(0, 8192);
			cR->setSingleStep(2);
			cropGrid->addWidget(cR, 1, 1);
			cropGrid->addWidget(new QLabel("B:"), 1, 2);
			QSpinBox *cB = new QSpinBox();
			cB->setRange(0, 8192);
			cB->setSingleStep(2);
			cropGrid->addWidget(cB, 1, 3);
			epLayout->addLayout(cropGrid);

			auto cropChanged = [this, i]() { onCropChanged(i); };
			connect(cL, QOverload<int>::of(&QSpinBox::valueChanged),
				this, cropChanged);
			connect(cT, QOverload<int>::of(&QSpinBox::valueChanged),
				this, cropChanged);
			connect(cR, QOverload<int>::of(&QSpinBox::valueChanged),
				this, cropChanged);
			connect(cB, QOverload<int>::of(&QSpinBox::valueChanged),
				this, cropChanged);

			QPushButton *removeBtn = new QPushButton("Remove this shot");
			removeBtn->setStyleSheet(
				"QPushButton { font-size: 11px; padding: 4px 8px; color: #e88; }"
				"QPushButton:hover { color: #fff; background: #7a2a2a; }");
			connect(removeBtn, &QPushButton::clicked, this,
				[this, i]() { onRemovePreset(i); });
			epLayout->addWidget(removeBtn);

			rowContainerLayout->addWidget(editPanel);
			presetsLayout->addWidget(rowContainer);

			PresetRow pr = {goBtn, cutBtn, capBtn, editBtn,
					editPanel, pDur, transCb, nameEd,
					cL, cT, cR, cB,
					posX, posY, scX, scY, rot,
					alignCb, btCb, bW, bH, bAlignCb,
					removeBtn};
			presetRows.append(pr);
		}
	}

	/* Update button labels & spinbox values */
	for (int i = 0; i < count && i < presetRows.size(); i++) {
		const char *name = shot_presets_get_name(i);
		bool active = shot_presets_is_active(i);
		QString label = QString("%1%2")
			.arg(active ? "" : "(empty) ")
			.arg(name && name[0] ? name : "Untitled");
		presetRows[i].goBtn->setText(label);
		presetRows[i].goBtn->setEnabled(active);
		presetRows[i].cutBtn->setEnabled(active);

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

		int cl, ct, cr, cb;
		shot_presets_get_crop(i, &cl, &ct, &cr, &cb);
		QSpinBox *sL = presetRows[i].cropLSpin;
		QSpinBox *sT = presetRows[i].cropTSpin;
		QSpinBox *sR = presetRows[i].cropRSpin;
		QSpinBox *sB = presetRows[i].cropBSpin;
		if (sL && !sL->hasFocus() && sL->value() != cl) {
			QSignalBlocker b(sL); sL->setValue(cl);
		}
		if (sT && !sT->hasFocus() && sT->value() != ct) {
			QSignalBlocker b(sT); sT->setValue(ct);
		}
		if (sR && !sR->hasFocus() && sR->value() != cr) {
			QSignalBlocker b(sR); sR->setValue(cr);
		}
		if (sB && !sB->hasFocus() && sB->value() != cb) {
			QSignalBlocker b(sB); sB->setValue(cb);
		}

		/* Populate the transform fields without firing change
		 * signals (QSignalBlocker) and without stealing keyboard
		 * focus mid-edit (hasFocus check). */
		shot_preset_transform_t xf;
		shot_presets_get_transform(i, &xf);
		auto setD = [](QDoubleSpinBox *w, double v) {
			if (!w || w->hasFocus()) return;
			if (w->value() == v) return;
			QSignalBlocker b(w);
			w->setValue(v);
		};
		auto setCb = [](QComboBox *w, int data) {
			if (!w || w->hasFocus()) return;
			int idx = w->findData(data);
			if (idx < 0) idx = 0;
			if (w->currentIndex() == idx) return;
			QSignalBlocker b(w);
			w->setCurrentIndex(idx);
		};
		setD(presetRows[i].posXSpin, xf.pos_x);
		setD(presetRows[i].posYSpin, xf.pos_y);
		setD(presetRows[i].scaleXSpin, xf.scale_x);
		setD(presetRows[i].scaleYSpin, xf.scale_y);
		setD(presetRows[i].rotSpin, xf.rotation);
		setCb(presetRows[i].alignCb, (int)xf.alignment);
		setCb(presetRows[i].boundsTypeCb, xf.bounds_type);
		setD(presetRows[i].boundsWSpin, xf.bounds_x);
		setD(presetRows[i].boundsHSpin, xf.bounds_y);
		setCb(presetRows[i].boundsAlignCb, (int)xf.bounds_align);
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

void ShotPresetsDock::onCropChanged(int index)
{
	if (index < 0 || index >= presetRows.size())
		return;
	int l = presetRows[index].cropLSpin->value();
	int t = presetRows[index].cropTSpin->value();
	int r = presetRows[index].cropRSpin->value();
	int b = presetRows[index].cropBSpin->value();
	shot_presets_set_crop(index, l, t, r, b);
}

void ShotPresetsDock::onTransitionChanged(int index, int type)
{
	shot_presets_set_transition(index, type);
}

void ShotPresetsDock::onTransformChanged(int index)
{
	if (index < 0 || index >= presetRows.size())
		return;
	PresetRow &r = presetRows[index];
	shot_preset_transform_t xf = {0};
	xf.pos_x    = (float)r.posXSpin->value();
	xf.pos_y    = (float)r.posYSpin->value();
	xf.scale_x  = (float)r.scaleXSpin->value();
	xf.scale_y  = (float)r.scaleYSpin->value();
	xf.rotation = (float)r.rotSpin->value();
	xf.alignment    = (unsigned int)r.alignCb->currentData().toInt();
	xf.bounds_type  = r.boundsTypeCb->currentData().toInt();
	xf.bounds_x     = (float)r.boundsWSpin->value();
	xf.bounds_y     = (float)r.boundsHSpin->value();
	xf.bounds_align = (unsigned int)r.boundsAlignCb->currentData().toInt();
	shot_presets_set_transform(index, &xf);
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

void ShotPresetsDock::onCaptureClicked(int index)
{
	shot_presets_capture(index);
	refreshUI();
}

void ShotPresetsDock::onDurationChanged(int value)
{
	shot_presets_set_duration(value);
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
