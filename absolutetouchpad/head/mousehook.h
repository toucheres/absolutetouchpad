#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN

#include <functional>

#include <Windows.h>

class MouseHook
{
	public:
		using Callback = std::function<bool(WPARAM, const MSLLHOOKSTRUCT&)>;

		explicit MouseHook(Callback callback);
		~MouseHook();

		bool install();
		void uninstall();
		bool isInstalled() const
		{
				return m_hook != nullptr;
		}

	private:
		static LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam);

		Callback m_callback;
		HHOOK m_hook = nullptr;
};

#endif
