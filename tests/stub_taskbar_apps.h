#pragma once
// Link stubs for app clicks through the taskbar's own buttons (taskbar_apps.h), for the
// tests that build the app model (apps_press.cpp) without taskbar_apps.cpp: the four dock tests (through
// dock_fixture_taskbar.h), apps_model_live_test, start_ghost_live_test. No press can be started, so every click goes
// the dock's own way and the real taskbar is never pressed. Include it where the test's other link stubs are, after
// the types (taskbarapps::Job, Result, Outcome: src\taskbar_apps_logic.h, which apps.h includes). It includes no
// src\ header itself: a live test includes taskbar_apps.h only later, through apps_click.cpp, and keeps that order.
namespace taskbarapps {
bool Begin(const Job&) { return false; }
bool Take(uint32_t, Result&) { return false; }
const wchar_t* OutcomeText(Outcome) { return L""; }
}
