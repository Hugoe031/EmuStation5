#include "launcherTheme.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QObject>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTimer>
#include <QWidget>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// PlayStation-style blue accent, used for focus rings, selection and links.
constexpr int ACCENT_RED   = 0x00;
constexpr int ACCENT_GREEN = 0x70;
constexpr int ACCENT_BLUE  = 0xd1;

QString g_dark_stylesheet;
QString g_light_stylesheet;

QString LoadStyleSheet(const char* path) {
	QFile style_sheet_file(QString::fromUtf8(path));
	if (style_sheet_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return QString::fromUtf8(style_sheet_file.readAll());
	}
	return {};
}

#if defined(_WIN32)
// Ask DWM to paint the native caption to match the application theme.
void ApplyTitleBarAttributes(BOOL dark, COLORREF caption) {
	auto* dwm = LoadLibraryW(L"dwmapi.dll");
	if (dwm == nullptr) {
		return;
	}
	using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
	const auto set_attribute =
	    reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
	if (set_attribute == nullptr) {
		return;
	}
	constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
	constexpr DWORD DWMWA_BORDER_COLOR            = 34;
	constexpr DWORD DWMWA_CAPTION_COLOR           = 35;
	for (auto* widget: QApplication::topLevelWidgets()) {
		if (widget == nullptr || !widget->isWindow()) {
			continue;
		}
		const auto handle = reinterpret_cast<HWND>(widget->winId());
		set_attribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
		set_attribute(handle, DWMWA_BORDER_COLOR, &caption, sizeof(caption));
		set_attribute(handle, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
	}
}

void ApplyDarkTitleBar() {
	ApplyTitleBarAttributes(TRUE, RGB(0x0e, 0x11, 0x17));
}

void ApplyLightTitleBar() {
	ApplyTitleBarAttributes(FALSE, RGB(0xF7, 0xF7, 0xF7));
}
#endif

class PaletteChangeFilter final: public QObject {
public:
	explicit PaletteChangeFilter(QApplication& application)
	    : QObject(&application), m_application(application) {}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override {
		if (event->type() == QEvent::ApplicationPaletteChange && !m_refresh_pending) {
			m_refresh_pending = true;
			QTimer::singleShot(0, &m_application, [this] {
				m_application.setStyleSheet(m_application.styleSheet());
				m_refresh_pending = false;
			});
		}

		return QObject::eventFilter(watched, event);
	}

private:
	QApplication& m_application;
	bool          m_refresh_pending = false;
};

QPalette DarkPalette() {
	QPalette palette;
	const QColor text(0xf2, 0xf2, 0xf2);
	const QColor muted(0x9a, 0x9a, 0x9a);
	const QColor disabled(0x6e, 0x6e, 0x6e);

	// Deep blue-charcoal with a faint blue tint, matching the PS5 home screen.
	palette.setColor(QPalette::Window, QColor(0x0e, 0x11, 0x17));
	palette.setColor(QPalette::WindowText, text);
	palette.setColor(QPalette::Base, QColor(0x18, 0x1d, 0x27));
	palette.setColor(QPalette::AlternateBase, QColor(0x1e, 0x24, 0x30));
	palette.setColor(QPalette::ToolTipBase, QColor(0x18, 0x1d, 0x27));
	palette.setColor(QPalette::ToolTipText, text);
	palette.setColor(QPalette::Text, text);
	palette.setColor(QPalette::PlaceholderText, muted);
	palette.setColor(QPalette::Button, QColor(0x1b, 0x21, 0x2b));
	palette.setColor(QPalette::ButtonText, text);
	palette.setColor(QPalette::BrightText, Qt::white);
	palette.setColor(QPalette::Light, QColor(0x35, 0x3e, 0x4d));
	palette.setColor(QPalette::Midlight, QColor(0x27, 0x2e, 0x3a));
	palette.setColor(QPalette::Mid, QColor(0x2a, 0x31, 0x3d));
	palette.setColor(QPalette::Dark, QColor(0x0b, 0x0e, 0x13));
	palette.setColor(QPalette::Shadow, Qt::black);
	palette.setColor(QPalette::Link, QColor(ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE));
	palette.setColor(QPalette::LinkVisited, QColor(0x7a, 0xb8, 0xf0));
	palette.setColor(QPalette::Highlight, QColor(ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE));
	palette.setColor(QPalette::HighlightedText, Qt::white);

	for (auto role: {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
		palette.setColor(QPalette::Disabled, role, disabled);
	}
	palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x33, 0x33, 0x33));
	palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
	palette.setColor(QPalette::Disabled, QPalette::Base, QColor(0x0e, 0x11, 0x17));

	return palette;
}

} // namespace

namespace LauncherTheme {

void Initialize(QApplication& application) {
	g_dark_stylesheet      = LoadStyleSheet(":/styles/launcher.qss");
	g_light_stylesheet     = LoadStyleSheet(":/styles/launcher-light.qss");
	if (!g_dark_stylesheet.isEmpty()) {
		application.setStyleSheet(g_dark_stylesheet);
		application.installEventFilter(new PaletteChangeFilter(application));
	}
}

void Apply(int theme) {
	if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
		QApplication::setStyle(fusion);
	}

	int effective = theme;
	if (effective == 0) {
		effective = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark ? 1 : 2;
	}

	const auto style_sheet = effective == 1 ? g_dark_stylesheet : g_light_stylesheet;
	if (!style_sheet.isEmpty()) {
		if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
			app->setStyleSheet(style_sheet);
		}
	}

	if (effective == 1) {
		QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
		QApplication::setPalette(DarkPalette());
#if defined(_WIN32)
		QTimer::singleShot(0, qApp, ApplyDarkTitleBar);
#endif
	} else {
		QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
		if (auto* style = QApplication::style()) {
			QApplication::setPalette(style->standardPalette());
		}
#if defined(_WIN32)
		QTimer::singleShot(0, qApp, ApplyLightTitleBar);
#endif
	}
}

} // namespace LauncherTheme
