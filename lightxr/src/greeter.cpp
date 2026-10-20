// SPDX-License-Identifier: GPL-3.0-or-later
extern "C" {
#include <lightdm.h>
}
#include <QtWidgets/QtWidgets>
#include <QtNetwork/QLocalSocket>
#include <QtCore/QtEndian>
#include <deque>

static constexpr int Width=1280, Height=800;
static QByteArray packet(const QByteArray& data) {
    QByteArray result(4, '\0');qToBigEndian<quint32>(data.size(), result.data());return result+data;
}
class Background : public QWidget {
    QPixmap art{":/lightxr/openxr-login-background.png"};
public:
    using QWidget::QWidget;
    void paintEvent(QPaintEvent*) override {QPainter p(this);p.fillRect(rect(),QColor("#111923"));p.drawPixmap(rect(),art);}
};
class Pointer : public QWidget {
public:
    Pointer(QWidget* p):QWidget(p){setAttribute(Qt::WA_TransparentForMouseEvents);resize(22,28);hide();}
    void paintEvent(QPaintEvent*) override {QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(QColor("#142133"),2));p.setBrush(QColor("#b4d9ff"));p.drawPolygon(QPolygon{{2,2},{2,23},{8,17},{13,27},{17,25},{12,15},{21,15}});}
};
class Greeter : public QWidget {
    LightDMGreeter* daemon=nullptr;
    bool demo=false, connected=false, authenticating=false, pending=false, starting=false;
    struct Prompt {QString text; bool secret;};
    std::deque<Prompt> prompts;
    QLineEdit *username, *answer;
    QLabel *message, *promptLabel, *sessionLabel, *xrStatus;
    QPushButton *submit, *sessionButton;
    QStringList sessions, sessionNames;
    int selectedSession=0;
    QWidget* root;
    Pointer* cursor;
    QPointer<QWidget> pressed;
    Qt::MouseButtons buttons{};
    QLocalSocket bridge;
    QByteArray incoming;
    QString bridgePath;
    QTimer frames, reconnect;
    bool shifted=false;
    QList<QPushButton*> letters;
    void send(const QByteArray& value) {
        if(bridge.state()==QLocalSocket::ConnectedState && bridge.bytesToWrite()<2*Width*Height*4)bridge.write(packet(value));
    }
    void clearAnswer() {answer->clear();}
    void status(const QString& text) {message->setText(text.toHtmlEscaped());}
    void showPrompt() {
        if(prompts.empty())return;
        promptLabel->setText(prompts.front().text);
        answer->setEchoMode(prompts.front().secret?QLineEdit::Password:QLineEdit::Normal);
        clearAnswer();answer->setEnabled(true);submit->setEnabled(true);submit->setText("Continue");
        answer->setFocus();
    }
    void idle(const QString& text) {
        prompts.clear();clearAnswer();authenticating=false;pending=false;starting=false;
        username->setEnabled(connected||demo);answer->setEnabled(false);
        submit->setEnabled(connected||demo);submit->setText("Sign in");
        promptLabel->setText("Your password will be requested securely.");status(text);username->setFocus();
    }
    void applySession() { sessionLabel->setText(sessionNames.value(selectedSession,"DomeD")); }
    void complete() {
        clearAnswer();prompts.clear();pending=false;
        if(!lightdm_greeter_get_is_authenticated(daemon)) {idle("Authentication was not accepted. Please try again.");return;}
        starting=true;status("Starting "+sessionNames.value(selectedSession)+"…");submit->setEnabled(false);
        // Stop sending/rendering the greeter before the authenticated user session starts.
        frames.stop();send("H");bridge.flush();
        lightdm_greeter_start_session(daemon,sessions.value(selectedSession,"dome").toUtf8().constData(),nullptr,
          [](GObject* obj,GAsyncResult* result,gpointer data){
            auto self=static_cast<Greeter*>(data);GError* error=nullptr;
            if(!lightdm_greeter_start_session_finish(LIGHTDM_GREETER(obj),result,&error)) {
                self->idle(error?QString::fromUtf8(error->message):"Unable to start session");
                if(error)g_error_free(error);
                self->frames.start(100);
            }
          },this);
    }
    void signIn() {
        if(starting||pending)return;
        if(demo) {status("Preview only · authentication is disabled.");return;}
        if(!connected)return;
        if(sessions.isEmpty()){status("No installed desktop sessions are available.");return;}
        GError* error=nullptr;
        if(!authenticating) {
            auto user=username->text().trimmed().toUtf8();
            if(user.isEmpty()){status("Enter your account name.");username->setFocus();return;}
            username->setEnabled(false);authenticating=true;submit->setEnabled(false);status("Authenticating…");
            if(!lightdm_greeter_authenticate(daemon,user.constData(),&error))idle("Unable to start authentication.");
        } else if(!prompts.empty()) {
            auto response=answer->text().toUtf8();clearAnswer();prompts.pop_front();submit->setEnabled(false);answer->setEnabled(false);
            bool ok=lightdm_greeter_respond(daemon,response.constData(),&error);
            response.fill('\0');
            if(!ok)idle("Unable to send authentication response.");
            else if(!prompts.empty())showPrompt();
        }
        if(error){status(QString::fromUtf8(error->message));g_error_free(error);}
    }
    void key(int code,const QString& text={},Qt::KeyboardModifiers modifiers={}) {
        auto target=QApplication::focusWidget();
        if(!target||!root->isAncestorOf(target))target=username;
        QKeyEvent down(QEvent::KeyPress,code,modifiers,text),up(QEvent::KeyRelease,code,modifiers,text);
        QApplication::sendEvent(target,&down);QApplication::sendEvent(target,&up);
    }
    void input(const QJsonObject& value) {
        auto type=value["type"].toString();
        if(type=="key") {key(value["key"].toInt(),value["text"].toString(),Qt::KeyboardModifiers(value["mods"].toInt()));return;}
        QPointF point(value["x"].toDouble(-1),value["y"].toDouble(-1));
        if(point.x()<0||point.x()>=Width||point.y()<0||point.y()>=Height)return;
        cursor->move(point.toPoint());cursor->raise();cursor->show();
        auto button=value["button"].toInt(1)==2?Qt::RightButton:Qt::LeftButton;
        QWidget* target=root->childAt(point.toPoint());if(!target)target=root;
        QEvent::Type event=QEvent::MouseMove;
        if(type=="down") {event=QEvent::MouseButtonPress;buttons|=button;pressed=target;}
        else if(type=="up") {event=QEvent::MouseButtonRelease;buttons&=~button;if(pressed)target=pressed;}
        else if(type!="move")return;
        if(type=="move"&&pressed)target=pressed;
        auto local=target->mapFrom(root,point);
        QMouseEvent mouse(event,local,target->mapToGlobal(local),event==QEvent::MouseMove?Qt::NoButton:button,buttons,Qt::NoModifier);
        QApplication::sendEvent(target,&mouse);
        if(type=="up")pressed=nullptr;
    }
protected:
    bool eventFilter(QObject* watched,QEvent* event) override {
        if(event->type()==QEvent::MouseMove||event->type()==QEvent::MouseButtonPress||event->type()==QEvent::MouseButtonRelease) {
            auto widget=qobject_cast<QWidget*>(watched);
            if(widget&&(widget==root||root->isAncestorOf(widget))) {
                auto point=widget->mapTo(root,static_cast<QMouseEvent*>(event)->position()).toPoint();
                cursor->move(point);cursor->raise();cursor->show();
            }
        }
        return QWidget::eventFilter(watched,event);
    }
public:
    Greeter(bool preview,const QString& endpoint):demo(preview),bridgePath(endpoint) {
        setWindowTitle("LightXR");setStyleSheet("QWidget{background:#111923;color:#edf3fb;font:20px 'DejaVu Sans';} QLabel{background:transparent;} QLineEdit{background:#202f41;border:1px solid #47617e;border-radius:10px;padding:12px;selection-background-color:#558bbd;} QLineEdit:focus{border:2px solid #94c9ff;} QPushButton{background:#24364b;border:1px solid #405a76;border-radius:9px;padding:10px;} QPushButton:hover{background:#34516f;} QPushButton:pressed{background:#446b94;} QPushButton:disabled{color:#76879b;} QPushButton#submit{background:#97caff;color:#0e1c2c;font-weight:600;}");
        root=new Background(this);root->setFixedSize(Width,Height);root->setObjectName("greeterPanel");
        auto outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);outer->addWidget(root,0,Qt::AlignCenter);
        auto layout=new QVBoxLayout(root);layout->setContentsMargins(56,36,56,28);layout->setSpacing(14);
        auto header=new QHBoxLayout;
        auto brand=new QLabel("LightXR");brand->setStyleSheet("font-size:36px;font-weight:600;color:#aed4ff;");header->addWidget(brand);header->addStretch();
        xrStatus=new QLabel("Local display · XR ready on connection");xrStatus->setStyleSheet("font-size:16px;color:#a5b8ce;");header->addWidget(xrStatus);layout->addLayout(header);
        auto body=new QHBoxLayout;body->setSpacing(60);
        auto welcome=new QVBoxLayout;welcome->addStretch(3);
        auto title=new QLabel("Welcome to DomeD.");title->setStyleSheet("font-size:30px;font-weight:500;");welcome->addWidget(title);
        auto subtitle=new QLabel("Sign in to DomeD\nWayland desktop + native XR");subtitle->setStyleSheet("font-size:21px;color:#a9bed5;");welcome->addWidget(subtitle);welcome->addSpacing(12);body->addLayout(welcome,1);
        auto card=new QWidget;card->setObjectName("loginCard");card->setStyleSheet("QWidget#loginCard{background:#111923;border-radius:14px;}");
        auto form=new QVBoxLayout(card);form->setContentsMargins(20,10,20,10);form->setSpacing(10);
        form->addWidget(new QLabel("Account"));username=new QLineEdit;username->setObjectName("username");username->setPlaceholderText("Username");username->setMaxLength(128);username->setContextMenuPolicy(Qt::NoContextMenu);form->addWidget(username);
        promptLabel=new QLabel("Your password will be requested securely.");promptLabel->setWordWrap(true);promptLabel->setStyleSheet("font-size:16px;color:#b7c8db;");form->addWidget(promptLabel);
        answer=new QLineEdit;answer->setObjectName("response");answer->setEchoMode(QLineEdit::Password);answer->setMaxLength(256);answer->setContextMenuPolicy(Qt::NoContextMenu);answer->setEnabled(false);form->addWidget(answer);
        auto sessionRow=new QHBoxLayout;sessionRow->addWidget(new QLabel("Session"));sessionLabel=new QLabel("DomeD");sessionLabel->setObjectName("session");sessionRow->addWidget(sessionLabel);sessionRow->addStretch();
        sessionButton=new QPushButton("Change");sessionButton->setFocusPolicy(Qt::NoFocus);sessionRow->addWidget(sessionButton);form->addLayout(sessionRow);
        submit=new QPushButton("Sign in");submit->setObjectName("submit");form->addWidget(submit);
        auto cancel=new QPushButton("Cancel authentication");cancel->setFocusPolicy(Qt::NoFocus);form->addWidget(cancel);
        message=new QLabel;message->setObjectName("status");message->setWordWrap(true);message->setMinimumHeight(52);message->setStyleSheet("font-size:16px;color:#b4cce6;");form->addWidget(message);body->addWidget(card,1);layout->addLayout(body,1);
        auto keyboard=new QVBoxLayout;keyboard->setSpacing(7);
        for(auto row:{QString("`1234567890-="),QString("qwertyuiop[]"),QString("asdfghjkl;'"),QString("zxcvbnm,./\\")}) {
            auto line=new QHBoxLayout;line->setSpacing(6);
            for(auto ch:row) {auto b=new QPushButton(QString(ch));b->setFocusPolicy(Qt::NoFocus);b->setMinimumHeight(42);b->setProperty("base",QString(ch));letters.append(b);line->addWidget(b);
                connect(b,&QPushButton::clicked,this,[this,b]{key(0,b->text());});}
            keyboard->addLayout(line);
        }
        auto bottom=new QHBoxLayout;
        for(auto label:{"Shift","Space","Backspace","Tab","Enter"}) {
            auto b=new QPushButton(label);b->setFocusPolicy(Qt::NoFocus);bottom->addWidget(b,QString(label)=="Space"?3:1);
            connect(b,&QPushButton::clicked,this,[this,label]{
                QString value(label);
                if(value=="Shift") {shifted=!shifted;const QString plain="`1234567890-=[];',./\\", upper="~!@#$%^&*()_+{}:\"<>?|";
                    for(auto b:letters){auto base=b->property("base").toString();auto index=plain.indexOf(base);b->setText(shifted?(index>=0?QString(upper[index]):base.toUpper()):base);} }
                else if(value=="Space")key(Qt::Key_Space," ");else if(value=="Backspace")key(Qt::Key_Backspace);
                else if(value=="Tab")key(Qt::Key_Tab);else signIn();
            });
        }
        keyboard->addLayout(bottom);layout->addLayout(keyboard);
        cursor=new Pointer(root);cursor->setObjectName("xrCursor");
        connect(submit,&QPushButton::clicked,this,[this]{signIn();});
        connect(username,&QLineEdit::returnPressed,this,[this]{signIn();});connect(answer,&QLineEdit::returnPressed,this,[this]{signIn();});
        connect(sessionButton,&QPushButton::clicked,this,[this]{if(!authenticating&&!sessions.empty()){selectedSession=(selectedSession+1)%sessions.size();applySession();}});
        connect(cancel,&QPushButton::clicked,this,[this]{if(daemon&&authenticating){GError* error=nullptr;lightdm_greeter_cancel_authentication(daemon,&error);if(error)g_error_free(error);clearAnswer();prompts.clear();submit->setEnabled(false);status("Cancelling…");}});
        for(GList* item=lightdm_get_sessions();item;item=item->next){auto s=LIGHTDM_SESSION(item->data);auto key=QString::fromUtf8(lightdm_session_get_key(s));if(sessions.contains(key))continue;sessions<<key;sessionNames<<QString::fromUtf8(lightdm_session_get_name(s));}
        selectedSession=sessions.indexOf("dome");if(selectedSession<0){if(demo&&sessions.isEmpty()){sessions<<"dome";sessionNames<<"DomeD";}selectedSession=0;}applySession();
        if(demo){connected=false;idle("Preview only · authentication is disabled.");}
        else {
            submit->setEnabled(false);username->setEnabled(false);status("Connecting to LightXR…");daemon=lightdm_greeter_new();
            g_signal_connect(daemon,"show-prompt",G_CALLBACK(+[](LightDMGreeter*,const gchar* text,LightDMPromptType type,gpointer data){auto self=static_cast<Greeter*>(data);self->prompts.push_back({QString::fromUtf8(text),type==LIGHTDM_PROMPT_TYPE_SECRET});if(self->prompts.size()==1)self->showPrompt();}),this);
            g_signal_connect(daemon,"show-message",G_CALLBACK(+[](LightDMGreeter*,const gchar* text,LightDMMessageType,gpointer data){static_cast<Greeter*>(data)->status(QString::fromUtf8(text));}),this);
            g_signal_connect(daemon,"authentication-complete",G_CALLBACK(+[](LightDMGreeter*,gpointer data){static_cast<Greeter*>(data)->complete();}),this);
            g_signal_connect(daemon,"idle",G_CALLBACK(+[](LightDMGreeter*,gpointer data){auto self=static_cast<Greeter*>(data);self->frames.stop();self->send("H");self->clearAnswer();}),this);
            g_signal_connect(daemon,"reset",G_CALLBACK(+[](LightDMGreeter*,gpointer data){auto self=static_cast<Greeter*>(data);self->idle("Sign in to continue.");self->frames.start(100);}),this);
            lightdm_greeter_connect_to_daemon(daemon,nullptr,[](GObject* obj,GAsyncResult* result,gpointer data){auto self=static_cast<Greeter*>(data);GError* error=nullptr;
                self->connected=lightdm_greeter_connect_to_daemon_finish(LIGHTDM_GREETER(obj),result,&error);
                self->idle(self->connected?"Sign in to continue.":"Display manager connection failed.");
                if(error)g_error_free(error);
                const char* selected=lightdm_greeter_get_select_user_hint(self->daemon);if(selected)self->username->setText(QString::fromUtf8(selected));
            },this);
        }
        auto glib=new QTimer(this);connect(glib,&QTimer::timeout,this,[]{for(int n=0;n<16&&g_main_context_pending(nullptr);++n)g_main_context_iteration(nullptr,FALSE);});glib->start(10);
        connect(&bridge,&QLocalSocket::connected,this,[this]{xrStatus->setText("XR bridge connected · 2 m curved panel");});
        connect(&bridge,&QLocalSocket::disconnected,this,[this]{incoming.clear();cursor->hide();xrStatus->setText("Local display · XR reconnecting");});
        connect(&bridge,&QLocalSocket::readyRead,this,[this]{
            incoming+=bridge.readAll();
            while(incoming.size()>=4){quint32 size=qFromBigEndian<quint32>(incoming.constData());if(!size||size>1024){bridge.abort();incoming.clear();return;}if(incoming.size()<4+size)return;
                auto event=incoming.mid(4,size);incoming.remove(0,4+size);if(event.startsWith('I'))input(QJsonDocument::fromJson(event.mid(1)).object());}
        });
        connect(&reconnect,&QTimer::timeout,this,[this]{if(bridge.state()==QLocalSocket::UnconnectedState)bridge.connectToServer(bridgePath);});reconnect.start(1000);
        connect(&frames,&QTimer::timeout,this,[this]{if(bridge.state()!=QLocalSocket::ConnectedState||bridge.bytesToWrite()>Width*Height*4)return;
            QImage image(Width,Height,QImage::Format_RGBA8888);image.fill(Qt::transparent);root->render(&image);
            QByteArray frame(9,'\0');frame[0]='F';qToBigEndian<quint32>(Width,frame.data()+1);qToBigEndian<quint32>(Height,frame.data()+5);
            frame.append(reinterpret_cast<const char*>(image.constBits()),image.sizeInBytes());send(frame);
        });frames.start(100);
        for(auto widget:root->findChildren<QWidget*>())widget->setMouseTracking(true);
        root->setMouseTracking(true);qApp->installEventFilter(this);
        resize(Width,Height);username->setFocus();
    }
    ~Greeter(){frames.stop();send("H");bridge.flush();bridge.disconnectFromServer();clearAnswer();if(daemon)g_object_unref(daemon);}
    QImage snapshot(){QImage image(Width,Height,QImage::Format_RGBA8888);image.fill(Qt::transparent);root->render(&image);return image;}
};
int main(int argc,char** argv){
    QApplication app(argc,argv);app.setApplicationName("LightXR");
    // Never enable GLib authentication debug logs (PAM messages may contain account data).
    QCommandLineParser p;p.addHelpOption();p.addOption({"demo","Preview without a LightDM/PAM connection"});
    p.addOption({"windowed","Windowed development preview"});p.addOption({"bridge","Seat broker socket","path","/run/lightxr-broker/bridge.sock"});
    p.addOption({"screenshot","Save a preview image and exit","path"});p.process(app);
    Greeter window(p.isSet("demo"),p.value("bridge"));if(p.isSet("windowed"))window.show();else window.showFullScreen();
    if(p.isSet("screenshot"))QTimer::singleShot(500,&app,[&]{app.exit(window.snapshot().save(p.value("screenshot"))?0:1);});
    return app.exec();
}
