/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
struct WindowTab;
struct CommentsPanel;

void CreateCommentsPanel(MainWindow*);
void DeleteCommentsPanel(MainWindow*);
ILayout* CommentsViewLayout(MainWindow*);
int CommentsViewControls(MainWindow*, ControlBase* out[3]);
HWND CommentsFocusHwnd(MainWindow*);
bool CanShowComments(WindowTab*);
void UpdateCommentsPanel(MainWindow*);
void RefreshCommentsPanel(MainWindow*);
void ClearCommentsPanel(MainWindow*);
void CommentsPanelSyncSelection(MainWindow*);
void UpdateCommentsPanelColors(MainWindow*);
void UpdateCommentsPanelDpi(MainWindow*, int dpi);
TempStr CommentsPanelTestTemp(MainWindow*, Str action, Str arg);
