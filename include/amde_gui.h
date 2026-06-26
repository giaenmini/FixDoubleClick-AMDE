/*
 * amde_gui.h — Config dialog + real-time bounce chart
 */
#pragma once
#ifndef AMDE_GUI_H
#define AMDE_GUI_H

#include "amde_core.h"

/* Tray icon */
void Tray_Add(HWND hwnd);
void Tray_Remove(void);
void Tray_UpdateTooltip(void);
void Tray_ShowContextMenu(HWND hwnd);

/* Config dialog  — modal, parent = NULL */
void GUI_ShowConfigDialog(HINSTANCE hInst);
extern HWND g_hCfgDlg;

/* Main message window */
LRESULT CALLBACK MsgWndProc(HWND, UINT, WPARAM, LPARAM);

/* Chart ring-buffer — written by hook, read by chart WndProc */
#define CHART_HISTORY   256     /* last N inter-click deltas to display */

typedef struct {
    uint32_t deltaUs[CHART_HISTORY];
    uint32_t thresholdUs[CHART_HISTORY]; /* threshold at that moment        */
    int      head;                       /* next-write index (ring)         */
    int      count;
    SRWLOCK  Lock;
} CHART_BUFFER;

extern CHART_BUFFER g_chart;

void Chart_Push(uint32_t deltaUs, uint32_t thresholdUs, BOOL wasBounce);

#endif /* AMDE_GUI_H */
