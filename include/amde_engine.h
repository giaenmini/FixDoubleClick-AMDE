/*
 * amde_engine.h — Debounce Engine API
 */
#pragma once
#ifndef AMDE_ENGINE_H
#define AMDE_ENGINE_H

#include "amde_core.h"

/* Lifecycle */
BOOL  Engine_Init(HINSTANCE hInstance);
void  Engine_Shutdown(void);

/* Profile registry */
DEVICE_PROFILE* Engine_GetOrCreateProfile(HANDLE hDevice);
void            Engine_ResetAllProfiles(void);

/* Persistence */
void Engine_PersistSave(void);
void Engine_PersistLoad(void);

/* Raw input window */
LRESULT CALLBACK RawInputWndProc(HWND, UINT, WPARAM, LPARAM);

/* Low-level mouse hook */
LRESULT CALLBACK BalancedMouseProc(int nCode, WPARAM wp, LPARAM lp);

/* Inline Bayesian helpers — used by engine and GUI */
static inline double Bayes_BounceProb(double alpha, double beta) {
    return alpha / (alpha + beta);
}

/*
 * Bayesian Beta conjugate update.
 *  bounce=TRUE  → clicked too fast, record as bounce observation
 *  bounce=FALSE → clean click, record as clean observation
 *
 * Using a forgetting factor λ=0.98 to slowly decay old evidence,
 * preventing the prior from becoming overconfident over long sessions.
 * Floor at initial values prevents prior from collapsing to 0.
 */
static inline void Bayes_Update(BUTTON_PERSISTENT* p, BOOL bounce) {
    const double lambda = 0.98;
    p->bayes_alpha = lambda * p->bayes_alpha + (bounce ? 1.0 : 0.0);
    p->bayes_beta  = lambda * p->bayes_beta  + (bounce ? 0.0 : 1.0);
    /* floor: keep at least Beta(1,9) mass to avoid over-confidence */
    if (p->bayes_alpha < BAYES_ALPHA_INIT) p->bayes_alpha = BAYES_ALPHA_INIT;
    if (p->bayes_beta  < BAYES_BETA_INIT)  p->bayes_beta  = BAYES_BETA_INIT;
}

#endif /* AMDE_ENGINE_H */
