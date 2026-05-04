#pragma once

#include <QWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVector>
#include <QTimer>
#include <QLineEdit>

class ShotPresetsDock : public QWidget {
	Q_OBJECT

public:
	explicit ShotPresetsDock(QWidget *parent = nullptr);

private slots:
	void onPresetClicked(int index);
	void onCutClicked(int index);
	void onFadeClicked(int index);
	void onCaptureClicked(int index);
	void onEditToggled(int index);
	void onDurationChanged(int value);
	void onFadeDurationChanged(int value);
	void onAtemSyncDelayChanged(int value);
	void onFadeSyncDelayChanged(int value);
	void onPresetDurationChanged(int index, int value);
	void onTransitionChanged(int index, int type);
	void onNameChanged(int index, const QString &name);
	void onAtemInputChanged(int index, int value);
	void onAddPreset();
	void onRemovePreset(int index);
	void onDefaultToggled(int index);
	void refreshUI();

private:
	void buildUI();

	QVBoxLayout *mainLayout;
	QSpinBox *durationSpin;
	QSpinBox *fadeDurationSpin;
	QSpinBox *atemSyncSpin;
	QSpinBox *fadeSyncSpin;
	QComboBox *easingTypeCb;
	QComboBox *easingFuncCb;
	QWidget *presetsContainer;
	QVBoxLayout *presetsLayout;
	QTimer *refreshTimer;
	QLabel *emptyLabel;
	QPushButton *addBtn;

	struct PresetRow {
		QPushButton *goBtn;
		QPushButton *fadeBtn;
		QPushButton *cutBtn;
		QPushButton *captureBtn;
		QPushButton *editBtn;
		QPushButton *defaultBtn;
		QWidget *editPanel;
		QSpinBox *presetDurSpin;
		QComboBox *transitionCb;
		QLineEdit *nameEdit;
		QSpinBox *atemInputSpin;
		QPushButton *removeBtn;
	};
	QVector<PresetRow> presetRows;
};

#ifdef __cplusplus
extern "C" {
#endif
void shot_presets_dock_init(void);
#ifdef __cplusplus
}
#endif
