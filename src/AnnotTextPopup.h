/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
struct Annotation;

// Read-only viewer for an annotation's full text. The hover tip shortens what
// it shows and the Edit PDF hover card shortens it further, so until now the
// only way to read a long comment was the editing UI (issue #4790).

// true if annot has text worth opening the popup for
bool AnnotationHasText(Annotation*);

// which of the card's edits gets the focus
enum class AnnotPopupFocus {
    Text,
    Reply,
};

// shows the popup for annot, anchored to it. Returns false if there is
// nothing to show.
bool ShowAnnotationTextPopup(MainWindow*, Annotation*, AnnotPopupFocus focus = AnnotPopupFocus::Text);
void HideAnnotationTextPopup(MainWindow*);
bool IsAnnotationTextPopupShown(MainWindow*);
bool IsAnnotationTextPopupShownFor(MainWindow*, Annotation*);
// keeps the popup on its annotation while the page scrolls or zooms
void RepositionAnnotationTextPopup(MainWindow*);
void DeleteAnnotationTextPopup(MainWindow*);
