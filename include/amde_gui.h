#pragma once
#ifndef AMDE_GUI_H
#define AMDE_GUI_H

#include "amde_core.h"

void Tray_Add(HWND hwnd);
void Tray_Remove(void);
void Tray_UpdateTooltip(void);
void Tray_ShowContextMenu(HWND hwnd);

void GUI_ShowConfigDialog(HINSTANCE hInst);
extern HWND g_hCfgDlg;

LRESULT CALLBACK MsgWndProc(HWND, UINT, WPARAM, LPARAM);

#define CHART_HISTORY   256     /* last N inter-click deltas to display */

typedef struct {
    uint32_t deltaUs[CHART_HISTORY];
    uint32_t thresholdUs[CHART_HISTORY]; 
    int      head;                       
    int      count;
    SRWLOCK  Lock;
} CHART_BUFFER;

extern CHART_BUFFER g_chart;

void Chart_Push(uint32_t deltaUs, uint32_t thresholdUs, BOOL wasBounce);

#endif
