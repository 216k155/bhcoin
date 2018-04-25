#ifndef LSRTOKEN_H
#define LSRTOKEN_H

#include "sendtokenpage.h"
#include "receivetokenpage.h"
#include "addtokenpage.h"

#include <QWidget>
#include <QModelIndex>

class TokenViewDelegate;
class WalletModel;
class QStandardItemModel;

namespace Ui {
class LSRToken;
}

class LSRToken : public QWidget
{
    Q_OBJECT

public:
    explicit LSRToken(QWidget *parent = 0);
    ~LSRToken();

    void setModel(WalletModel *_model);

Q_SIGNALS:

public Q_SLOTS:

    void on_addToken(QString address, QString name, QString symbol, int decimals, double balance);
    void on_goToSendTokenPage();
    void on_goToReceiveTokenPage();
    void on_goToAddTokenPage();

private:
    Ui::LSRToken *ui;
    SendTokenPage* m_sendTokenPage;
    ReceiveTokenPage* m_receiveTokenPage;
    AddTokenPage* m_addTokenPage;
    WalletModel* m_model;
    TokenViewDelegate* m_tokenDelegate;
    QStandardItemModel *m_tokenModel;
    QAction *m_sendAction;
    QAction *m_receiveAction;
    QAction *m_addTokenAction;
};

#endif // LSRTOKEN_H
