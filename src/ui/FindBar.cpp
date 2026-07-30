#include "FindBar.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QVBoxLayout>

FindBar::FindBar(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 4, 6, 4);
    outer->setSpacing(4);

    // --- Find row ---
    auto* findRow = new QHBoxLayout();
    findRow->setSpacing(6);

    m_term = new QLineEdit(this);
    m_term->setPlaceholderText(tr("Find"));
    m_term->setClearButtonEnabled(true);

    auto* prev = new QToolButton(this);
    prev->setText(QStringLiteral("▲"));
    prev->setToolTip(tr("Previous match (Shift+F3)"));

    auto* next = new QToolButton(this);
    next->setText(QStringLiteral("▼"));
    next->setToolTip(tr("Next match (F3)"));

    m_caseBox  = new QCheckBox(tr("Aa"), this);
    m_caseBox->setToolTip(tr("Match case"));
    m_regexBox = new QCheckBox(tr(".*"), this);
    m_regexBox->setToolTip(tr("Regular expression"));

    m_status = new QLabel(this);
    m_status->setMinimumWidth(120);

    auto* close = new QToolButton(this);
    close->setText(QStringLiteral("✕"));
    close->setToolTip(tr("Close (Esc)"));

    findRow->addWidget(new QLabel(tr("Find:"), this));
    findRow->addWidget(m_term, 1);
    findRow->addWidget(prev);
    findRow->addWidget(next);
    findRow->addWidget(m_caseBox);
    findRow->addWidget(m_regexBox);
    findRow->addWidget(m_status);
    findRow->addWidget(close);
    outer->addLayout(findRow);

    // --- Replace row ---
    auto* replaceRow = new QHBoxLayout();
    replaceRow->setSpacing(6);

    m_replaceLabel  = new QLabel(tr("Replace:"), this);
    m_replacement   = new QLineEdit(this);
    m_replacement->setPlaceholderText(tr("Replace with"));
    m_replaceBtn    = new QToolButton(this);
    m_replaceBtn->setText(tr("Replace"));
    m_replaceAllBtn = new QToolButton(this);
    m_replaceAllBtn->setText(tr("Replace All"));

    replaceRow->addWidget(m_replaceLabel);
    replaceRow->addWidget(m_replacement, 1);
    replaceRow->addWidget(m_replaceBtn);
    replaceRow->addWidget(m_replaceAllBtn);
    outer->addLayout(replaceRow);

    connect(next,  &QToolButton::clicked, this, &FindBar::findNext);
    connect(prev,  &QToolButton::clicked, this, &FindBar::findPrevious);
    connect(close, &QToolButton::clicked, this, &FindBar::dismissed);
    connect(m_replaceBtn,    &QToolButton::clicked, this, &FindBar::replaceCurrent);
    connect(m_replaceAllBtn, &QToolButton::clicked, this, &FindBar::replaceAll);
    connect(m_term, &QLineEdit::returnPressed, this, &FindBar::findNext);
    // Incremental search as the user types.
    connect(m_term, &QLineEdit::textChanged, this, &FindBar::optionsChanged);
    connect(m_caseBox,  &QCheckBox::toggled, this, &FindBar::optionsChanged);
    connect(m_regexBox, &QCheckBox::toggled, this, &FindBar::optionsChanged);
    connect(m_replacement, &QLineEdit::returnPressed, this, &FindBar::replaceCurrent);
}

QString FindBar::searchTerm()    const { return m_term->text(); }
QString FindBar::replacement()   const { return m_replacement->text(); }
bool    FindBar::caseSensitive() const { return m_caseBox->isChecked(); }
bool    FindBar::useRegex()      const { return m_regexBox->isChecked(); }

void FindBar::activate(bool withReplace, const QString& seed)
{
    m_replaceLabel->setVisible(withReplace);
    m_replacement->setVisible(withReplace);
    m_replaceBtn->setVisible(withReplace);
    m_replaceAllBtn->setVisible(withReplace);

    if (!seed.isEmpty() && !seed.contains(QLatin1Char('\n')))
        m_term->setText(seed);

    show();
    m_term->setFocus();
    m_term->selectAll();
}

void FindBar::setStatus(const QString& text, bool notFound)
{
    m_status->setText(text);
    m_status->setStyleSheet(notFound ? QStringLiteral("color: #cc0000;") : QString());
}

void FindBar::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) { emit dismissed(); return; }
    QWidget::keyPressEvent(e);
}
