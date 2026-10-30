#include "credentialdialogs.h"
#include "credentialstore.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>
#include <functional>

namespace {
class BusyCredentialDialog final:public QDialog {
public:
    using QDialog::QDialog;
    void reject() override {}
};
void credentialWork(QWidget *parent,const QString &message,const std::function<void()> &work)
{
    BusyCredentialDialog progress(parent);progress.setWindowTitle("API key protection");
    progress.setWindowModality(Qt::ApplicationModal);progress.setMinimumWidth(340);
    auto layout=new QVBoxLayout(&progress);layout->addWidget(new QLabel(message));
    auto bar=new QProgressBar;bar->setRange(0,0);layout->addWidget(bar);
    QThread *thread=QThread::create(work);
    QObject::connect(thread,&QThread::finished,&progress,&QDialog::accept);
    thread->start();progress.exec();thread->wait();delete thread;
}
QLineEdit *secretField(const QString &name)
{
    auto field=new QLineEdit;field->setObjectName(name);field->setEchoMode(QLineEdit::Password);
    field->setInputMethodHints(Qt::ImhHiddenText|Qt::ImhNoPredictiveText|Qt::ImhNoAutoUppercase);
    return field;
}
}

bool unlockCredentialForProfile(QWidget *parent,const QJsonObject &profile,
    const QString &directory,QString *apiKey,const QByteArray &staged)
{
    if(!apiKey)return false;
    QDialog dialog(parent);dialog.setObjectName("unlockCredentialDialog");dialog.setWindowTitle("Unlock saved API key");dialog.setMinimumWidth(400);
    auto layout=new QVBoxLayout(&dialog);
    auto hint=new QLabel(QString("Unlock the saved key for %1. It remains available until you lock it or close SpeechDesk.").arg(profile.value("name").toString("this profile")));hint->setWordWrap(true);layout->addWidget(hint);
    auto form=new QFormLayout;auto pass=secretField("unlockPassphrase");form->addRow("Passphrase",pass);layout->addLayout(form);
    auto errorLabel=new QLabel;errorLabel->setObjectName("credentialError");errorLabel->setWordWrap(true);layout->addWidget(errorLabel);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText("Unlock");layout->addWidget(buttons);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        if(pass->text().isEmpty()){errorLabel->setText("Enter the passphrase for this saved key.");return;}
        QByteArray phrase=pass->text().toUtf8(),plain;QString error;bool ok=false;
        pass->clear();
        credentialWork(&dialog,"Unlocking the selected profile…",[&]{
            try {
                if(staged.isEmpty()){CredentialStore store(directory);ok=store.unlock(profile,phrase,&plain,&error);}
                else ok=CredentialStore::decrypt(profile,staged,phrase,&plain,&error);
            } catch(...) {error="The credential could not be unlocked.";}
        });
        CredentialStore::wipe(phrase);
        if(!ok){CredentialStore::wipe(plain);errorLabel->setText(error.isEmpty()?"The passphrase is incorrect or this saved key cannot be read.":error);pass->setFocus();return;}
        *apiKey=QString::fromUtf8(plain);CredentialStore::wipe(plain);dialog.accept();
    });
    pass->setFocus();const bool accepted=dialog.exec()==QDialog::Accepted;pass->clear();return accepted;
}

bool protectCredentialForProfile(QWidget *parent,const QJsonObject &profile,
    const QString &currentKey,bool replacement,QByteArray *encrypted,QString *apiKey)
{
    if(!encrypted||!apiKey)return false;
    QDialog dialog(parent);dialog.setObjectName("protectCredentialDialog");dialog.setWindowTitle(replacement?"Replace saved API key":"Save encrypted API key");dialog.setMinimumWidth(460);
    auto layout=new QVBoxLayout(&dialog);
    auto hint=new QLabel(replacement
        ?"Enter the same or a new API key and choose a new passphrase. Saving Settings replaces this profile's old encrypted key. Other profiles are unaffected; the old passphrase is not needed."
        :"Choose a passphrase to encrypt this profile's API key. The encrypted key is written only when you save Settings. The passphrase is never saved.");
    hint->setWordWrap(true);layout->addWidget(hint);
    auto form=new QFormLayout;auto key=secretField("replacementApiKey");key->setText(currentKey);form->addRow("API key",key);
    auto pass=secretField("newPassphrase");form->addRow("New passphrase",pass);
    auto confirm=secretField("confirmPassphrase");form->addRow("Confirm passphrase",confirm);layout->addLayout(form);
    auto note=new QLabel("Use a long, unique passphrase of at least 12 characters. If you forget the passphrase, replace the saved key with your API key and a new passphrase.");note->setWordWrap(true);note->setProperty("muted",true);layout->addWidget(note);
    auto errorLabel=new QLabel;errorLabel->setObjectName("credentialError");errorLabel->setWordWrap(true);layout->addWidget(errorLabel);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Save)->setText(replacement?"Prepare replacement":"Protect key");layout->addWidget(buttons);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        if(key->text().trimmed().isEmpty()){errorLabel->setText("Enter the API key to protect.");return;}
        if(pass->text().size()<12){errorLabel->setText("Use a passphrase of at least 12 characters.");return;}
        if(pass->text()!=confirm->text()){errorLabel->setText("The passphrases do not match.");return;}
        QByteArray plain=key->text().trimmed().toUtf8(),phrase=pass->text().toUtf8(),result;QString error;
        pass->clear();confirm->clear();
        credentialWork(&dialog,"Encrypting the selected profile's API key…",[&]{
            try {result=CredentialStore::encrypt(profile,plain,phrase,&error);}
            catch(...) {error="The credential could not be encrypted.";}
        });
        CredentialStore::wipe(phrase);
        if(result.isEmpty()){CredentialStore::wipe(plain);errorLabel->setText(error.isEmpty()?"Could not protect the API key.":error);pass->setFocus();return;}
        *encrypted=result;*apiKey=QString::fromUtf8(plain);CredentialStore::wipe(plain);dialog.accept();
    });
    if(currentKey.isEmpty())key->setFocus();else pass->setFocus();
    const bool accepted=dialog.exec()==QDialog::Accepted;key->clear();pass->clear();confirm->clear();return accepted;
}
