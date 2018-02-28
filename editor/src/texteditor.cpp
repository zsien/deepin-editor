/* -*- Mode: C++; indent-tabs-mode: nil; tab-width: 4 -*-
 * -*- coding: utf-8 -*-
 *
 * Copyright (C) 2011 ~ 2018 Deepin, Inc.
 *               2011 ~ 2018 Wang Yong
 *
 * Author:     Wang Yong <wangyong@deepin.com>
 * Maintainer: Wang Yong <wangyong@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "texteditor.h"
#include "utils.h"
#include "window.h"

#include "Definition"
#include "SyntaxHighlighter"
#include "Theme"

#include <DDesktopServices>
#include <QApplication>
#include <DSettingsGroup>
#include <DSettingsOption>
#include <DSettings>
#include <QClipboard>
#include <QFileInfo>
#include <QDebug>
#include <QPainter>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTextBlock>
#include <QTimer>

class LineNumberArea : public QWidget
{
public:
    LineNumberArea(TextEditor *editor)
        : QWidget(editor),
          editor(editor) {
    }

    void paintEvent(QPaintEvent *event) {
        editor->lineNumberAreaPaintEvent(event);
    }

    TextEditor *editor;
};

TextEditor::TextEditor(QPlainTextEdit *parent) :
    QPlainTextEdit(parent),
    m_highlighter(new KSyntaxHighlighting::SyntaxHighlighter(document()))
{
    // Don't draw background.
    viewport()->setAutoFillBackground(false);

    // Init highlight theme.
    setTheme((palette().color(QPalette::Base).lightness() < 128)
             ? m_repository.defaultTheme(KSyntaxHighlighting::Repository::DarkTheme)
             : m_repository.defaultTheme(KSyntaxHighlighting::Repository::LightTheme));

    // Init widgets.
    lineNumberArea = new LineNumberArea(this);

    connect(this, &QPlainTextEdit::updateRequest, this, &TextEditor::handleUpdateRequest);
    connect(this, &QPlainTextEdit::textChanged, this, &TextEditor::updateLineNumber, Qt::QueuedConnection);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &TextEditor::highlightCurrentLine, Qt::QueuedConnection);

    // Init menu.
    rightMenu = new QMenu();
    rightMenu->setStyle(QStyleFactory::create("dlight"));
    undoAction = new QAction("Undo", this);
    redoAction = new QAction("Redo", this);
    cutAction = new QAction("Cut", this);
    copyAction = new QAction("Copy", this);
    pasteAction = new QAction("Paste", this);
    deleteAction = new QAction("Delete", this);
    selectAllAction = new QAction("Select All", this);
    findAction = new QAction("Find", this);
    replaceAction = new QAction("Replace", this);
    jumpLineAction = new QAction("Jump line", this);
    fullscreenAction = new QAction("Fullscreen", this);
    exitFullscreenAction = new QAction("Exit fullscreen", this);
    openInFileManagerAction = new QAction("Open in file manager", this);

    connect(rightMenu, &QMenu::aboutToHide, this, &TextEditor::removeHighlightWordUnderCursor);
    connect(undoAction, &QAction::triggered, this, &TextEditor::undo);
    connect(redoAction, &QAction::triggered, this, &TextEditor::redo);
    connect(cutAction, &QAction::triggered, this, &TextEditor::clickCutAction);
    connect(copyAction, &QAction::triggered, this, &TextEditor::clickCopyAction);
    connect(pasteAction, &QAction::triggered, this, &TextEditor::clickPasteAction);
    connect(deleteAction, &QAction::triggered, this, &TextEditor::clickDeleteAction);
    connect(selectAllAction, &QAction::triggered, this, &TextEditor::selectAll);
    connect(findAction, &QAction::triggered, this, &TextEditor::clickFindAction);
    connect(replaceAction, &QAction::triggered, this, &TextEditor::clickReplaceAction);
    connect(jumpLineAction, &QAction::triggered, this, &TextEditor::clickJumpLineAction);
    connect(fullscreenAction, &QAction::triggered, this, &TextEditor::clickFullscreenAction);
    connect(exitFullscreenAction, &QAction::triggered, this, &TextEditor::clickFullscreenAction);
    connect(openInFileManagerAction, &QAction::triggered, this, &TextEditor::clickOpenInFileManagerAction);

    // Init convert case sub menu.
    haveWordUnderCursor = false;
    convertCaseMenu = new QMenu("Convert Case");
    upcaseAction = new QAction("Upcase", this);
    downcaseAction = new QAction("Downcase", this);
    capitalizeAction = new QAction("Capitalize", this);

    convertCaseMenu->addAction(upcaseAction);
    convertCaseMenu->addAction(downcaseAction);
    convertCaseMenu->addAction(capitalizeAction);

    connect(upcaseAction, &QAction::triggered, this, &TextEditor::upcaseWord);
    connect(downcaseAction, &QAction::triggered, this, &TextEditor::downcaseWord);
    connect(capitalizeAction, &QAction::triggered, this, &TextEditor::capitalizeWord);

    canUndo = false;
    canRedo = false;

    connect(this, &TextEditor::undoAvailable, this,
            [=] (bool undoIsAvailable) {
                canUndo = undoIsAvailable;
            });
    connect(this, &TextEditor::redoAvailable, this,
            [=] (bool redoIsAvailable) {
                canRedo = redoIsAvailable;
            });

    // Init scroll animation.
    scrollAnimation = new QPropertyAnimation(verticalScrollBar(), "value");
    scrollAnimation->setEasingCurve(QEasingCurve::InOutExpo);
    scrollAnimation->setDuration(300);

    connect(scrollAnimation, &QPropertyAnimation::finished, this, &TextEditor::handleScrollFinish, Qt::QueuedConnection);

    // Highlight line and focus.
    highlightCurrentLine();
    QTimer::singleShot(0, this, SLOT(setFocus()));
}

int TextEditor::getCurrentLine()
{
    return textCursor().blockNumber() + 1;
}

int TextEditor::getCurrentColumn()
{
    return textCursor().columnNumber();
}

int TextEditor::getPosition()
{
    return textCursor().position();
}

int TextEditor::getScrollOffset()
{
    QScrollBar *scrollbar = verticalScrollBar();

    return scrollbar->value();
}

void TextEditor::forwardChar()
{
    moveCursor(QTextCursor::NextCharacter);
}

void TextEditor::backwardChar()
{
    moveCursor(QTextCursor::PreviousCharacter);
}

void TextEditor::forwardWord()
{
    moveCursor(QTextCursor::NextWord);
}

void TextEditor::backwardWord()
{
    moveCursor(QTextCursor::PreviousWord);
}

void TextEditor::forwardPair()
{
    if (find(QRegExp("[\"'>)}]"))) {
        QTextCursor cursor = textCursor();
        cursor.clearSelection();

        setTextCursor(cursor);
    }
}

void TextEditor::backwardPair()
{
    QTextDocument::FindFlags options;
    options |= QTextDocument::FindBackward;

    if (find(QRegExp("[\"'<({]"), options)) {
        QTextCursor cursor = textCursor();
        cursor.clearSelection();
        cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor);

        setTextCursor(cursor);
    }
}

void TextEditor::moveToStartOfLine()
{
    moveCursor(QTextCursor::StartOfLine);
}

void TextEditor::moveToEndOfLine()
{
    moveCursor(QTextCursor::EndOfLine);
}

void TextEditor::moveToLineIndentation()
{
    // Get line start position.
    moveCursor(QTextCursor::StartOfLine);
    int startColumn = textCursor().columnNumber();

    // Get line end position.
    moveCursor(QTextCursor::EndOfLine);
    int endColumn = textCursor().columnNumber();

    // Move to line start first.
    moveCursor(QTextCursor::StartOfLine);

    // Move to first non-blank char of line.
    int column = startColumn;
    while (column <= endColumn) {
        QChar currentChar = toPlainText().at(std::max(textCursor().position() - 1, 0));

        if (!currentChar.isSpace()) {
            moveCursor(QTextCursor::PreviousCharacter);
            break;
        } else {
            moveCursor(QTextCursor::NextCharacter);
        }

        column++;
    }
}

void TextEditor::nextLine()
{
    moveCursor(QTextCursor::Down);
}

void TextEditor::prevLine()
{
    moveCursor(QTextCursor::Up);
}

void TextEditor::jumpToLine(int line, bool keepLineAtCenter)
{
    QTextCursor cursor(document()->findBlockByLineNumber(line - 1)); // line - 1 because line number starts from 0

    // Update cursor.
    setTextCursor(cursor);

    if (keepLineAtCenter) {
        keepCurrentLineAtCenter();
    }
}

void TextEditor::newline()
{
    QTextCursor cursor = textCursor();
    cursor.insertText("\n");
    setTextCursor(cursor);
}

void TextEditor::openNewlineAbove()
{
    moveCursor(QTextCursor::StartOfLine);
    textCursor().insertText("\n");
    prevLine();
}

void TextEditor::openNewlineBelow()
{
    moveCursor(QTextCursor::EndOfLine);
    textCursor().insertText("\n");
}

void TextEditor::swapLineUp()
{
    if (textCursor().hasSelection()) {
        // Get selection bound.
        int startPos = textCursor().anchor();
        int endPos = textCursor().position();

        if (startPos > endPos) {
            std::swap(startPos, endPos);
        }

        // Expand selection to multi-lines bound.
        QTextCursor startCursor = textCursor();
        startCursor.setPosition(startPos, QTextCursor::MoveAnchor);
        startCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);

        QTextCursor endCursor = textCursor();
        endCursor.setPosition(endPos, QTextCursor::MoveAnchor);
        endCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        QTextCursor cursor = textCursor();
        cursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
        cursor.setPosition(endCursor.position(), QTextCursor::KeepAnchor);

        setTextCursor(cursor);

        // Get multi-line selection text.
        QString newTop = cursor.selectedText();
        cursor.removeSelectedText();

        // Get one-line content above multi-lines selection.
        cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString newBottom = cursor.selectedText();
        cursor.removeSelectedText();

        // Record new selection bound of multi-lines.
        int newSelectionStartPos = cursor.position();
        cursor.insertText(newTop);
        int newSelectionEndPos = cursor.position();
        cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor);
        cursor.insertText(newBottom);

        // Reset multi-line selection status.
        cursor.setPosition(newSelectionStartPos, QTextCursor::MoveAnchor);
        cursor.setPosition(newSelectionEndPos, QTextCursor::KeepAnchor);

        // Update cursor.
        setTextCursor(cursor);
    } else {
        QTextCursor cursor = textCursor();

        // Rember current line's column number.
        int column = cursor.columnNumber();

        // Get current line content.
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString newTop = cursor.selectedText();
        cursor.removeSelectedText();

        // Get line content above current line.
        // Note: we need move cursor UP and then use *StartOfBlock*, keep this order.
        // don't use *StartOfBlock* before *UP*, it's won't work if above line is *wrap line*.
        cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString newBottom = cursor.selectedText();
        cursor.removeSelectedText();

        // Swap line content.
        cursor.insertText(newTop);
        cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor);
        cursor.insertText(newBottom);

        // Move cursor to new start of line.
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);

        // Restore cursor's column.
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);

        // Update cursor.
        setTextCursor(cursor);
    }
}

void TextEditor::swapLineDown()
{
    if (textCursor().hasSelection()) {
        // Get selection bound.
        int startPos = textCursor().anchor();
        int endPos = textCursor().position();

        if (startPos > endPos) {
            std::swap(startPos, endPos);
        }

        // Expand selection to lines bound.
        QTextCursor startCursor = textCursor();
        startCursor.setPosition(startPos, QTextCursor::MoveAnchor);
        startCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);

        QTextCursor endCursor = textCursor();
        endCursor.setPosition(endPos, QTextCursor::MoveAnchor);
        endCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        QTextCursor cursor = textCursor();
        cursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
        cursor.setPosition(endCursor.position(), QTextCursor::KeepAnchor);

        setTextCursor(cursor);

        // Get multi-line selection content.
        QString newBottom = cursor.selectedText();
        cursor.removeSelectedText();

        // Get line content below multi-line selection.
        cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString newTop = cursor.selectedText();
        cursor.removeSelectedText();

        // Record new selection bound after swap content.
        cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);
        cursor.insertText(newTop);

        cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor);
        int newSelectionStartPos = cursor.position();
        cursor.insertText(newBottom);
        int newSelectionEndPos = cursor.position();

        // Reset selection bound for multi-line content.
        cursor.setPosition(newSelectionStartPos, QTextCursor::MoveAnchor);
        cursor.setPosition(newSelectionEndPos, QTextCursor::KeepAnchor);

        // Update cursor.
        setTextCursor(cursor);
    } else {
        QTextCursor cursor = textCursor();

        // Rember current line's column number.
        int column = cursor.columnNumber();

        // Get current line content.
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString newBottom = cursor.selectedText();
        cursor.removeSelectedText();

        // Get line content below current line.
        cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString newTop = cursor.selectedText();
        cursor.removeSelectedText();

        // Swap content.
        cursor.insertText(newBottom);
        cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);
        cursor.insertText(newTop);

        // Move new start of line.
        cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        // Restore cursor's column.
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);

        // Update cursor.
        setTextCursor(cursor);
    }
}

void TextEditor::scrollLineUp()
{
    QScrollBar *scrollbar = verticalScrollBar();
    if (scrollbar->value() + 2 >= getCurrentLine()) {
        jumpToLine(getCurrentLine() + 1, false);
    }
    scrollbar->setValue(scrollbar->value() + 1);
}

void TextEditor::scrollLineDown()
{
    QScrollBar *scrollbar = verticalScrollBar();
    int visibleLines = (rect().height() / cursorRect().height());
    if (scrollbar->value() + visibleLines <= getCurrentLine() + 2) {
        jumpToLine(getCurrentLine() - 1, false);
    }
    
    scrollbar->setValue(scrollbar->value() - 1);
}

void TextEditor::duplicateLine()
{
    // Rember current line's column number.
    int column = textCursor().columnNumber();

    // Get current line's content.
    QTextCursor cursor(textCursor().block());
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    QString text = cursor.selectedText();

    // Copy current line.
    cursor.movePosition(QTextCursor::EndOfLine, QTextCursor::MoveAnchor);
    cursor.insertText("\n");
    cursor.insertText(text);

    // Restore cursor's column.
    cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);

    // Update cursor.
    setTextCursor(cursor);
}

void TextEditor::killLine()
{
    // Remove selection content if has selection.
    if (textCursor().hasSelection()) {
        textCursor().removeSelectedText();
    } else {
        // Get current line content.
        QTextCursor cursor(textCursor().block());
        cursor.movePosition(QTextCursor::StartOfBlock);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString text = cursor.selectedText();

        // Cursor is at end of line.
        bool isEmptyLine = text.size() == 0;

        // Join next line if current line is empty or cursor at end of line.
        if (isEmptyLine || textCursor().atBlockEnd()) {
            QTextCursor cursor = textCursor();

            cursor.movePosition(QTextCursor::EndOfLine, QTextCursor::MoveAnchor);
            cursor.deleteChar();

            // Update cursor.
            setTextCursor(cursor);
        }
        // Otherwise kill rest content of line.
        else {
            QTextCursor cursor = textCursor();

            cursor.movePosition(QTextCursor::NoMove, QTextCursor::KeepAnchor);
            cursor.movePosition(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();

            // Update cursor.
            setTextCursor(cursor);
        }
    }
}

void TextEditor::killCurrentLine()
{
    QTextCursor cursor = textCursor();

    cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();

    setTextCursor(cursor);
}

void TextEditor::killBackwardWord()
{
    if (textCursor().hasSelection()) {
        textCursor().removeSelectedText();
    } else {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();

        setTextCursor(cursor);
    }
}

void TextEditor::killForwardWord()
{
    if (textCursor().hasSelection()) {
        textCursor().removeSelectedText();
    } else {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();

        setTextCursor(cursor);
    }
}

void TextEditor::indentLine()
{
    // Save cursor column.
    int column = getCurrentColumn();

    // If current column is not Multiples of 4, jump to 4x position before next indent.
    moveToLineIndentation();
    int indentSpace = tabSpaceNumber - (getCurrentColumn() % tabSpaceNumber);

    // Insert spaces.
    moveToStartOfLine();
    QString spaces(indentSpace, ' ');
    textCursor().insertText(spaces);

    // Restore cursor column postion.
    moveToStartOfLine();
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column + indentSpace);
    setTextCursor(cursor);
}

void TextEditor::backIndentLine()
{
    // Save cursor column.
    int column = getCurrentColumn();

    // If current column is not Multiples of 4, jump to 4x position before back indent.
    moveToLineIndentation();

    if (getCurrentColumn() > 0) {
        int indentSpace = getCurrentColumn() % tabSpaceNumber;
        if (indentSpace == 0 && getCurrentColumn() >= tabSpaceNumber) {
            indentSpace = tabSpaceNumber;
        }

        // Remove spaces.
        QTextCursor deleteCursor = textCursor();
        for (int i = 0; i < indentSpace; i++) {
            deleteCursor.deletePreviousChar();
        }
        setTextCursor(deleteCursor);

        // Restore cursor column postion.
        moveToStartOfLine();
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column - indentSpace);
        setTextCursor(cursor);
    }
}

void TextEditor::setTabSpaceNumber(int number)
{
    tabSpaceNumber = number;
}

void TextEditor::upcaseWord()
{
    convertWordCase(UPPER);
}

void TextEditor::downcaseWord()
{
    convertWordCase(LOWER);
}

void TextEditor::capitalizeWord()
{
    convertWordCase(CAPITALIZE);
}

void TextEditor::transposeChar()
{
    QTextCursor cursor = textCursor();
    cursor.clearSelection();

    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QString nextChar = cursor.selectedText();
    cursor.removeSelectedText();

    cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
    QString prevChar = cursor.selectedText();
    cursor.removeSelectedText();

    cursor.insertText(nextChar);
    cursor.insertText(prevChar);

    if (!nextChar.isEmpty()) {
        cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor);
    }

    setTextCursor(cursor);
}

void TextEditor::convertWordCase(ConvertCase convertCase)
{
    if (textCursor().hasSelection()) {
        QString text = textCursor().selectedText();

        if (convertCase == UPPER) {
            textCursor().insertText(text.toUpper());
        } else if (convertCase == LOWER) {
            textCursor().insertText(text.toLower());
        } else {
            text = text.toLower();
            text.replace(0, 1, text[0].toUpper());
            textCursor().insertText(text);
        }
    } else {
        QTextCursor cursor;

        // Move cursor to mouse position first. if have word under mouse pointer.
        if (haveWordUnderCursor) {
            setTextCursor(wordUnderPointerCursor);
        }

        cursor = textCursor();
        cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);

        QString text = cursor.selectedText();
        if (convertCase == UPPER) {
            cursor.insertText(text.toUpper());
        } else if (convertCase == LOWER) {
            cursor.insertText(text.toLower());
        } else {
            text = text.toLower();
            text.replace(0, 1, text[0].toUpper());
            cursor.insertText(text);
        }

        setTextCursor(cursor);

        haveWordUnderCursor = false;
    }
}

void TextEditor::keepCurrentLineAtCenter()
{
    QScrollBar *scrollbar = verticalScrollBar();

    int currentLine = cursorRect().top() / cursorRect().height();
    int halfEditorLines = rect().height() / 2 / cursorRect().height();
    scrollbar->setValue(scrollbar->value() + currentLine - halfEditorLines);
}

void TextEditor::scrollToLine(int scrollOffset, int row, int column)
{
    // Save cursor postion.
    restoreRow = row;
    restoreColumn = column;

    // Start scroll animation.
    scrollAnimation->setStartValue(verticalScrollBar()->value());
    scrollAnimation->setEndValue(scrollOffset);
    scrollAnimation->start();
}

void TextEditor::setFontFamily(QString fontName)
{
    QTextDocument *doc = document();
    QFont font = doc->defaultFont();
    font.setFixedPitch(true);
    font.setFamily(fontName);
    doc->setDefaultFont(font);
}

void TextEditor::setFontSize(int size)
{
    // Update font.
    QTextDocument *doc = document();
    QFont font = doc->defaultFont();
    font.setFixedPitch(true);
    font.setPointSize(size);
    doc->setDefaultFont(font);

    // Update line number after adjust font size.
    updateLineNumber();
}

void TextEditor::replaceAll(QString replaceText, QString withText)
{
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    QString text = cursor.selectedText();

    cursor.insertText(text.replace(replaceText, withText));
    cursor.clearSelection();

    // Update cursor.
    setTextCursor(cursor);

    highlightKeyword(replaceText, getPosition());
}

void TextEditor::replaceNext(QString replaceText, QString withText)
{
    QTextCursor cursor = textCursor();

    cursor.setPosition(cursorKeywordSelection.cursor.position() - replaceText.size());
    cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, replaceText.size());
    cursor.insertText(withText);

    // Update cursor.
    setTextCursor(cursor);

    highlightKeyword(replaceText, getPosition());
}

void TextEditor::replaceRest(QString replaceText, QString withText)
{
    QTextCursor cursor = textCursor();
    cursor.setPosition(cursorKeywordSelection.cursor.position() - replaceText.size());
    cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    QString text = cursor.selectedText();

    cursor.insertText(text.replace(replaceText, withText));
    cursor.clearSelection();

    // Update cursor.
    setTextCursor(cursor);

    highlightKeyword(replaceText, getPosition());
}

void TextEditor::removeKeywords()
{
    cursorKeywordSelection.cursor = textCursor();

    keywordSelections.clear();

    updateHighlightLineSeleciton();

    renderAllSelections();

    setFocus();
}

void TextEditor::highlightKeyword(QString keyword, int position)
{
    updateKeywordSelections(keyword);

    updateCursorKeywordSelection(position, true);

    updateHighlightLineSeleciton();

    renderAllSelections();
}

void TextEditor::updateCursorKeywordSelection(int position, bool findNext)
{
    bool findOne = setCursorKeywordSeletoin(position, findNext);

    if (!findOne) {
        if (findNext) {
            setCursorKeywordSeletoin(0, findNext);
        } else {
            QTextCursor cursor = textCursor();
            cursor.movePosition(QTextCursor::End, QTextCursor::MoveAnchor);

            setCursorKeywordSeletoin(cursor.position(), findNext);
        }
    }
}

void TextEditor::updateHighlightLineSeleciton()
{
    QTextEdit::ExtraSelection selection;

    QColor lineColor = QColor("#333333");

    selection.format.setBackground(lineColor);
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    selection.cursor = textCursor();
    selection.cursor.clearSelection();

    currentLineSelection = selection;
}

void TextEditor::updateKeywordSelections(QString keyword)
{
    // Clear keyword selections first.
    keywordSelections.clear();

    // Update selections with keyword.
    if (keyword != "") {
        moveCursor(QTextCursor::Start);

        while(find(keyword)) {
            QTextEdit::ExtraSelection extra;

            QPen outline(QColor("#D33D6D").lighter(120), 1, Qt::SolidLine);
            extra.format.setProperty(QTextFormat::OutlinePen, outline);

            QBrush brush(QColor("#303030"));
            extra.format.setProperty(QTextFormat::BackgroundBrush, brush);

            extra.cursor = textCursor();
            keywordSelections.append(extra);
        }

        setExtraSelections(keywordSelections);
    }
}

void TextEditor::renderAllSelections()
{
    QList<QTextEdit::ExtraSelection> selections;

    selections.append(currentLineSelection);
    selections.append(keywordSelections);
    selections.append(cursorKeywordSelection);
    selections.append(wordUnderCursorSelection);

    setExtraSelections(selections);
}

void TextEditor::keyPressEvent(QKeyEvent *keyEvent)
{
    QString key = Utils::getKeyshortcut(keyEvent);

    // qDebug() << "\n***********" << key;

    if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "indentline")) {
        indentLine();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "backindentline")) {
        backIndentLine();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "forwardchar")) {
        forwardChar();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "backwardchar")) {
        backwardChar();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "forwardword")) {
        forwardWord();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "backwardword")) {
        backwardWord();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "nextline")) {
        nextLine();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "prevline")) {
        prevLine();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "newline")) {
        newline();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "opennewlineabove")) {
        openNewlineAbove();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "opennewlinebelow")) {
        openNewlineBelow();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "duplicateline")) {
        duplicateLine();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "killline")) {
        killLine();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "killcurrentline")) {
        killCurrentLine();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "swaplineup")) {
        swapLineUp();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "swaplinedown")) {
        swapLineDown();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "scrolllineup")) {
        scrollLineUp();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "scrolllinedown")) {
        scrollLineDown();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "movetoendofline")) {
        moveToEndOfLine();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "movetostartofline")) {
        moveToStartOfLine();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "movetolineindentation")) {
        moveToLineIndentation();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "upcaseword")) {
        upcaseWord();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "downcaseword")) {
        downcaseWord();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "capitalizeword")) {
        capitalizeWord();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "killbackwardword")) {
        killBackwardWord();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "killforwardword")) {
        killForwardWord();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "forwardpair")) {
        forwardPair();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "backwardpair")) {
        backwardPair();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "transposechar")) {
        transposeChar();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "selectall")) {
        selectAll();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "copy")) {
        copySelectText();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "cut")) {
        cut();
    } else if (key == Utils::getKeyshortcutFromKeymap(settings, "editor", "paste")) {
        paste();
    } else {
        // Post event to window widget if key match window key list.
        for (auto option : settings->settings->group("shortcuts.window")->options()) {
            if (key == settings->settings->option(option->key())->value().toString()) {
                keyEvent->ignore();
                return;
            }
        }

        // Text editor handle key self.
        QPlainTextEdit::keyPressEvent(keyEvent);
    }
}

void TextEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    // Init.
    QPainter painter(lineNumberArea);
    painter.fillRect(event->rect(), QColor("#202020"));

    QColor lineColor = QColor("#202020");
    lineColor.setAlphaF(0.05);
    painter.fillRect(QRect(event->rect().x() + event->rect().width() - 1, event->rect().y(), 1, event->rect().height()), lineColor);

    // Update line number.
    QTextBlock block = firstVisibleBlock();
    int top = (int) blockBoundingGeometry(block).translated(contentOffset()).top();
    int bottom = top + (int) blockBoundingRect(block).height();
    int linenumber = block.blockNumber();

    Utils::setFontSize(painter, document()->defaultFont().pointSize() - 1);
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(QColor("#666666"));
            painter.drawText(0,
                             top + lineNumberOffset,
                             lineNumberArea->width(),
                             fontMetrics().height(),
                             Qt::AlignHCenter | Qt::AlignBottom,
                             QString::number(linenumber + 1));
        }

        block = block.next();
        top = bottom;
        bottom = top + (int) blockBoundingRect(block).height();

        ++linenumber;
    }
}

void TextEditor::contextMenuEvent(QContextMenuEvent *event)
{
    rightMenu->clear();

    if (canUndo) {
        rightMenu->addAction(undoAction);
    }
    if (canRedo) {
        rightMenu->addAction(redoAction);
    }
    rightMenu->addSeparator();
    if (textCursor().hasSelection()) {
        rightMenu->addAction(cutAction);
        rightMenu->addAction(copyAction);
    } else {
        // Just show copy/cut menu item when cursor rectangle contain moue pointer coordinate.
        haveWordUnderCursor = highlightWordUnderMouse(event->pos());
        if (haveWordUnderCursor) {
            rightMenu->addAction(cutAction);
            rightMenu->addAction(copyAction);
        }
    }
    if (canPaste()) {
        rightMenu->addAction(pasteAction);
    }
    rightMenu->addAction(deleteAction);
    rightMenu->addAction(selectAllAction);
    rightMenu->addSeparator();
    rightMenu->addAction(findAction);
    rightMenu->addAction(replaceAction);
    rightMenu->addAction(jumpLineAction);
    rightMenu->addSeparator();
    rightMenu->addMenu(convertCaseMenu);
    rightMenu->addAction(openInFileManagerAction);
    rightMenu->addSeparator();
    if (static_cast<Window*>(this->window())->isFullScreen()) {
        rightMenu->addAction(exitFullscreenAction);
    } else {
        rightMenu->addAction(fullscreenAction);
    }

    rightMenu->exec(event->globalPos());
}

void TextEditor::highlightCurrentLine()
{
    updateHighlightLineSeleciton();
    renderAllSelections();
}

void TextEditor::updateLineNumber()
{
    lineNumberArea->setFixedWidth(QString("%1").arg(blockCount()).size() * fontMetrics().width('9') + lineNumberPaddingX * 2);
}

void TextEditor::handleScrollFinish()
{
    // Restore cursor postion.
    jumpToLine(restoreRow, false);

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, restoreColumn);

    // Update cursor.
    setTextCursor(cursor);
}

void TextEditor::handleUpdateRequest(const QRect &rect, int dy)
{
    if (dy) {
        lineNumberArea->scroll(0, dy);
    } else {
        lineNumberArea->update(0, rect.y(), lineNumberArea->width(), rect.height());
    }
}

bool TextEditor::setCursorKeywordSeletoin(int position, bool findNext)
{
    if (findNext) {
        for (int i = 0; i < keywordSelections.size(); i++) {
            if (keywordSelections[i].cursor.position() > position) {
                cursorKeywordSelection.cursor = keywordSelections[i].cursor;

                QBrush brush(QColor("#FF6347"));
                cursorKeywordSelection.format.setProperty(QTextFormat::BackgroundBrush, brush);

                jumpToLine(keywordSelections[i].cursor.blockNumber() + 1, false);

                QTextCursor cursor = textCursor();
                cursor.setPosition(keywordSelections[i].cursor.position());

                // Update cursor.
                setTextCursor(cursor);

                return true;
            }
        }
    } else {
        for (int i = keywordSelections.size() - 1; i >= 0; i--) {
            if (keywordSelections[i].cursor.position() < position) {
                cursorKeywordSelection.cursor = keywordSelections[i].cursor;

                QBrush brush(QColor("#FF6347"));
                cursorKeywordSelection.format.setProperty(QTextFormat::BackgroundBrush, brush);

                jumpToLine(keywordSelections[i].cursor.blockNumber() + 1, false);

                QTextCursor cursor = textCursor();
                cursor.setPosition(keywordSelections[i].cursor.position());

                // Update cursor.
                setTextCursor(cursor);

                return true;
            }
        }
    }

    return false;
}

void TextEditor::setTheme(const KSyntaxHighlighting::Theme &theme)
{
    auto pal = qApp->palette();
    if (theme.isValid()) {
        pal.setColor(QPalette::Base, theme.editorColor(KSyntaxHighlighting::Theme::BackgroundColor));
        pal.setColor(QPalette::Text, theme.textColor(KSyntaxHighlighting::Theme::Normal));
        pal.setColor(QPalette::Highlight, theme.editorColor(KSyntaxHighlighting::Theme::TextSelection));
    }
    setPalette(pal);

    m_highlighter->setTheme(theme);
    m_highlighter->rehighlight();
}

void TextEditor::loadHighlighter()
{
    const auto def = m_repository.definitionForFileName(QFileInfo(filepath).fileName());
    m_highlighter->setDefinition(def);
}

bool TextEditor::highlightWordUnderMouse(QPoint pos)
{
    // Get cursor match mouse pointer coordinate, but cursor maybe not under mouse pointer.
    QTextCursor cursor(cursorForPosition(pos));

    // Get cursor rectangle.
    auto rect = cursorRect(cursor);
    int widthOffset = 10;
    rect.setX(std::max(rect.x() - widthOffset / 2, 0));
    rect.setWidth(rect.width() + widthOffset);

    // Just highlight word under pointer when cursor rectangle contain moue pointer coordinate.
    if ((rect.x() <= pos.x()) &&
        (pos.x() <= rect.x() + rect.width()) &&
        (rect.y() <= pos.y()) &&
        (pos.y() <= rect.y() + rect.height())) {
        // Move back to word bound start postion, and save cursor for convert case.
        wordUnderPointerCursor = cursor;
        wordUnderPointerCursor.select(QTextCursor::WordUnderCursor);
        wordUnderPointerCursor.setPosition(wordUnderPointerCursor.anchor(), QTextCursor::MoveAnchor);

        // Update highlight cursor.
        QTextEdit::ExtraSelection selection;

        QColor lineColor = QColor("#660000");

        selection.format.setBackground(lineColor);
        selection.cursor = cursor;
        selection.cursor.select(QTextCursor::WordUnderCursor);

        wordUnderCursorSelection = selection;

        renderAllSelections();

        return true;
    } else {
        return false;
    }
}

void TextEditor::removeHighlightWordUnderCursor()
{
    highlightWordCacheCursor = wordUnderCursorSelection.cursor;

    QTextEdit::ExtraSelection selection;
    wordUnderCursorSelection = selection;

    renderAllSelections();
}

void TextEditor::setSettings(Settings *keySettings)
{
    settings = keySettings;
}

void TextEditor::copySelectText()
{
    copy();
    
    QTextCursor cursor = textCursor();
    cursor.clearSelection();
    setTextCursor(cursor);
}

void TextEditor::clickCutAction()
{
    if (textCursor().hasSelection()) {
        cut();
    } else {
        cutWordUnderCursor();
    }
}

void TextEditor::clickCopyAction()
{
    if (textCursor().hasSelection()) {
        copySelectText();
    } else {
        copyWordUnderCursor();
    }
}

void TextEditor::clickPasteAction()
{
    if (textCursor().hasSelection()) {
        paste();
    } else {
        setTextCursor(highlightWordCacheCursor);

        paste();
    }
}

void TextEditor::clickDeleteAction()
{
    if (textCursor().hasSelection()) {
        textCursor().removeSelectedText();
    } else {
        setTextCursor(highlightWordCacheCursor);
        textCursor().removeSelectedText();
    }
}

void TextEditor::clickOpenInFileManagerAction()
{
    DDesktopServices::showFileItem(filepath);
}

void TextEditor::copyWordUnderCursor()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(highlightWordCacheCursor.selectedText());
}

void TextEditor::cutWordUnderCursor()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(highlightWordCacheCursor.selectedText());

    setTextCursor(highlightWordCacheCursor);
    textCursor().removeSelectedText();
}
