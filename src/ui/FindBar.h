#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QToolButton;

// Inline search / replace bar docked at the bottom of the viewport (SRC-01).
// Owns no document state: it emits requests and MainWindow performs them.
class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(QWidget* parent = nullptr);

    QString searchTerm()    const;
    QString replacement()   const;
    bool    caseSensitive() const;
    bool    useRegex()      const;

    // Shows the bar; when `withReplace` the replacement row is visible too.
    void activate(bool withReplace, const QString& seed = {});
    void setStatus(const QString& text, bool notFound = false);

signals:
    void findNext();
    void findPrevious();
    void replaceCurrent();
    void replaceAll();
    void optionsChanged();
    void dismissed();

protected:
    void keyPressEvent(QKeyEvent*) override;

private:
    QLineEdit*   m_term        = nullptr;
    QLineEdit*   m_replacement = nullptr;
    QLabel*      m_replaceLabel = nullptr;
    QToolButton* m_replaceBtn  = nullptr;
    QToolButton* m_replaceAllBtn = nullptr;
    QCheckBox*   m_caseBox     = nullptr;
    QCheckBox*   m_regexBox    = nullptr;
    QLabel*      m_status      = nullptr;
};
