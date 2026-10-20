// Integration test against the real liblightdm greeter wire protocol; no PAM bypass in production.
#define main lightxrGreeterMain
#include "../src/greeter.cpp"
#undef main
#include <QtTest/QTest>
#include <QtNetwork/QLocalServer>
#include <sys/socket.h>
#include <fcntl.h>
#include <stdexcept>
#include <functional>
#include <iostream>
static void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
static QByteArray integer(quint32 n){QByteArray b(4,'\0');qToBigEndian<quint32>(n,b.data());return b;}
static QByteArray string(const QByteArray& b){return integer(b.size())+b;}
static QByteArray wire(quint32 id,const QByteArray& data){return integer(id)+integer(data.size())+data;}
static void wait(const std::function<bool()>& pred){for(int n=0;n<300;++n){QCoreApplication::processEvents();if(pred())return;QTest::qWait(10);}throw std::runtime_error("UI/protocol timed out");}
int main(int argc,char** argv)try{
    QApplication app(argc,argv);QTemporaryDir temp;require(temp.isValid(),"Temporary directory");
    int sockets[2];require(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets),"Protocol socket pair");
    qputenv("LIGHTDM_TO_SERVER_FD",QByteArray::number(sockets[1]));qputenv("LIGHTDM_FROM_SERVER_FD",QByteArray::number(sockets[1]));
    QLocalServer bridge;auto endpoint=temp.path()+"/bridge.sock";require(bridge.listen(endpoint),"Bridge fixture");
    QLocalSocket* peer=nullptr;QByteArray bridgeBytes;bool hidden=false;
    QObject::connect(&bridge,&QLocalServer::newConnection,&app,[&]{peer=bridge.nextPendingConnection();QObject::connect(peer,&QLocalSocket::readyRead,&app,[&]{bridgeBytes+=peer->readAll();while(bridgeBytes.size()>=4){auto n=qFromBigEndian<quint32>(bridgeBytes.constData());if(bridgeBytes.size()<4+n)break;auto data=bridgeBytes.mid(4,n);bridgeBytes.remove(0,4+n);if(data=="H")hidden=true;}});});
    int attempts=0,starts=0;quint32 sequence=0;QByteArray protocol;
    fcntl(sockets[0],F_SETFL,O_NONBLOCK);
    QSocketNotifier server(sockets[0],QSocketNotifier::Read);
    auto sendServer=[&](quint32 id,const QByteArray& body){auto data=wire(id,body);require(::write(sockets[0],data.data(),data.size())==data.size(),"Fixture protocol write");};
    QObject::connect(&server,&QSocketNotifier::activated,&app,[&]{
        char data[4096];auto n=::read(sockets[0],data,sizeof(data));if(n>0)protocol.append(data,n);
        while(protocol.size()>=8){quint32 id=qFromBigEndian<quint32>(protocol.constData()),size=qFromBigEndian<quint32>(protocol.constData()+4);if(protocol.size()<8+size)break;auto body=protocol.mid(8,size);protocol.remove(0,8+size);
            if(id==0)sendServer(7,integer(1)+string("test-lightdm")+integer(2)+string("default-session")+string("plasma")+string("select-user")+string("alice"));
            else if(id==1){++attempts;sequence=qFromBigEndian<quint32>(body.constData());require(body.mid(8)=="alice","Selected user must reach LightDM");
                sendServer(1,integer(sequence)+string("alice")+integer(2)+integer(1)+string("Password:")+integer(2)+string("One-time code:"));}
            else if(id==3){require(qFromBigEndian<quint32>(body.constData())==2,"Both PAM prompts must receive responses");require(body==integer(2)+string("test-secret")+string("123456"),"PAM response order and text");sendServer(2,integer(sequence)+string("alice")+integer(attempts>=3?0:7));}
            else if(id==5)sendServer(2,integer(sequence)+string("alice")+integer(26));
            else if(id==4){require(attempts==3,"Never start a session before successful authentication");require(body==string("dome"),"DomeD must be preset despite another default hint");++starts;sendServer(3,integer(0));}
            else throw std::runtime_error("Unexpected greeter protocol message");
        }
    });
    Greeter window(false,endpoint);window.show();
    auto username=window.findChild<QLineEdit*>("username"),answer=window.findChild<QLineEdit*>("response");auto submit=window.findChild<QPushButton*>("submit");
    wait([&]{return username->isEnabled()&&peer;});require(username->text()=="alice","Selected user hint");
    QTest::mouseMove(username,username->rect().center());
    auto xrCursor=window.findChild<QWidget*>("xrCursor");wait([&]{return xrCursor->isVisible();});
    auto remote=[&](QJsonObject value){peer->write(packet("I"+QJsonDocument(value).toJson(QJsonDocument::Compact)));peer->flush();};
    auto remoteClick=[&](QWidget* target){auto root=window.findChild<QWidget*>("greeterPanel");auto point=target->mapTo(root,target->rect().center());
        remote({{"type","down"},{"x",point.x()},{"y",point.y()},{"button",1}});remote({{"type","up"},{"x",point.x()},{"y",point.y()},{"button",1}});};
    remoteClick(submit);wait([&]{return answer->isEnabled();});require(starts==0&&answer->echoMode()==QLineEdit::Password,"Password prompt must be masked and login gated");
    auto namedButton=[&](const QString& label){for(auto button:window.findChildren<QPushButton*>())if(button->text()==label)return button;throw std::runtime_error("Missing keyboard key");};
    remoteClick(namedButton("q"));wait([&]{return answer->text()=="q";});
    remoteClick(namedButton("Shift"));wait([&]{return namedButton("Q")!=nullptr;});
    remoteClick(namedButton("Q"));wait([&]{return answer->text()=="qQ";});
    remoteClick(namedButton("Backspace"));wait([&]{return answer->text()=="q";});
    remoteClick(namedButton("Shift"));
    for(auto button:window.findChildren<QPushButton*>())if(button->text()=="Cancel authentication")QTest::mouseClick(button,Qt::LeftButton);
    wait([&]{return username->isEnabled();});require(starts==0&&answer->text().isEmpty(),"Cancellation clears secret and does not launch");
    for(int attempt=2;attempt<=3;++attempt){
        QTest::mouseClick(submit,Qt::LeftButton);wait([&]{return answer->isEnabled();});QTest::keyClicks(answer,"test-secret");QTest::keyClick(answer,Qt::Key_Return);
        wait([&]{return answer->isEnabled()&&answer->echoMode()==QLineEdit::Normal;});require(answer->text().isEmpty(),"Clear previous response before next PAM prompt");
        remote({{"type","key"},{"key",0},{"text","123456"}});wait([&]{return answer->text()=="123456";});QTest::keyClick(answer,Qt::Key_Return);
        if(attempt==2){wait([&]{return username->isEnabled();});require(starts==0&&answer->text().isEmpty(),"Failed authentication cannot launch or retain secret");}
    }
    wait([&]{return starts==1&&hidden;});require(answer->text().isEmpty(),"Clear responses before session handoff");
    window.snapshot().save(temp.path()+"/greeter.png");std::cout<<"PASS: real LightDM protocol, masked multi-prompt PAM flow, cancellation/failure, DomeD handoff, physical keys and XR mouse/keys\n";
    server.setEnabled(false);::close(sockets[0]);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
