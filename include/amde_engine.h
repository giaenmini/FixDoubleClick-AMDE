#pragma once
#ifndef AMDE_ENGINE_H
#define AMDE_ENGINE_H

#include "amde_core.h"

BOOL  Engine_Init(HINSTANCE hInstance);
void  Engine_Shutdown(void);

DEVICE_PROFILE* Engine_GetOrCreateProfile(HANDLE hDevice);
void            Engine_ResetAllProfiles(void);

void Engine_PersistSave(void);
void Engine_PersistLoad(void);

LRESULT CALLBACK RawInputWndProc(HWND, UINT, WPARAM, LPARAM);

LRESULT CALLBACK BalancedMouseProc(int nCode, WPARAM wp, LPARAM lp);

static inline double Bayes_BounceProb(double alpha, double beta) {
    return alpha / (alpha + beta);
}


static inline void Bayes_Update(BUTTON_PERSISTENT* p, BOOL bounce) {
    const double lambda = 0.98;
    p->bayes_alpha = lambda * p->bayes_alpha + (bounce ? 1.0 : 0.0);
    p->bayes_beta  = lambda * p->bayes_beta  + (bounce ? 0.0 : 1.0);
    /* floor: keep at least Beta(1,9) mass to avoid over-confidence */
    if (p->bayes_alpha < BAYES_ALPHA_INIT) p->bayes_alpha = BAYES_ALPHA_INIT;
    if (p->bayes_beta  < BAYES_BETA_INIT)  p->bayes_beta  = BAYES_BETA_INIT;
}

#endif
