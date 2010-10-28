/*
 * Copyright 2010, The Android Open Source Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebRequest.h"

#include "JNIUtility.h"
#include "MainThread.h"
#include "WebCoreFrameBridge.h"
#include "WebRequestContext.h"
#include "WebResourceRequest.h"
#include "jni.h"

#include <cutils/log.h>
#include <string>
#include <utils/AssetManager.h>

extern android::AssetManager* globalAssetManager();

// TODO:
// - Finish the file upload. Testcase is mobile buzz
// - Add network throttle needed by Android plugins

// TODO: Turn off asserts crashing before release
// http://b/issue?id=2951985
#undef ASSERT
#define ASSERT(assertion, ...) do \
    if (!(assertion)) { \
        android_printLog(ANDROID_LOG_ERROR, __FILE__, __VA_ARGS__); \
        CRASH(); \
    } \
while (0)

namespace android {

namespace {
    const int kInitialReadBufSize = 32768;
}

WebRequest::WebRequest(WebUrlLoaderClient* loader, const WebResourceRequest& webResourceRequest)
    : m_urlLoader(loader)
    , m_inputStream(0)
    , m_androidUrl(false)
    , m_url(webResourceRequest.url())
    , m_userAgent(webResourceRequest.userAgent())
    , m_loadState(Created)
{
    GURL gurl(m_url);

    m_request = new URLRequest(gurl, this);

    m_request->SetExtraRequestHeaders(webResourceRequest.requestHeaders());
    m_request->set_referrer(webResourceRequest.referrer());
    m_request->set_method(webResourceRequest.method());
}

// This is a special URL for Android. Query the Java InputStream
// for data and send to WebCore
WebRequest::WebRequest(WebUrlLoaderClient* loader, const WebResourceRequest& webResourceRequest, int inputStream)
    : m_urlLoader(loader)
    , m_androidUrl(true)
    , m_url(webResourceRequest.url())
    , m_userAgent(webResourceRequest.userAgent())
    , m_loadState(Created)
{
    JNIEnv* env = JSC::Bindings::getJNIEnv();
    m_inputStream = (int)env->NewGlobalRef((_jobject*)inputStream);
}

WebRequest::~WebRequest()
{
    ASSERT(m_loadState == Finished, "dtor called on a WebRequest in a different state than finished (%d)", m_loadState);

    m_loadState = Deleted;
    JNIEnv* env = JSC::Bindings::getJNIEnv();
    if (m_inputStream)
        env->DeleteGlobalRef((_jobject*)m_inputStream);
}

const std::string& WebRequest::getUrl() const
{
    return m_url;
}

const std::string& WebRequest::getUserAgent() const
{
    return m_userAgent;
}

void WebRequest::finish(bool success)
{
    ASSERT(m_loadState < Finished, "called finish on an already finished WebRequest (%d)", m_loadState);

    // Make sure WebUrlLoaderClient doesn't delete us in the middle of this method.
    scoped_refptr<WebRequest> guard(this);

    m_loadState = Finished;
    if (success) {
        m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
                m_urlLoader.get(), &WebUrlLoaderClient::didFinishLoading));
    } else {
        OwnPtr<WebResponse> webResponse(new WebResponse(m_request.get()));
        m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
                m_urlLoader.get(), &WebUrlLoaderClient::didFail, webResponse.release()));
    }
    m_networkBuffer = 0;
    m_request = 0;
    m_urlLoader = 0;
}

void WebRequest::AppendBytesToUpload(WTF::Vector<char>* data)
{
    // AppendBytesToUpload is only valid before calling start
    ASSERT(m_loadState == Created, "Start called on a WebRequest not in CREATED state: (%s)", m_url.c_str());
    m_request->AppendBytesToUpload(data->data(), data->size());
    delete data;
}

void WebRequest::start(bool isPrivateBrowsing)
{
    ASSERT(m_loadState == Created, "Start called on a WebRequest not in CREATED state: (%s)", m_url.c_str());

    m_loadState = Started;

    if (m_androidUrl)
        return handleAndroidURL();

    // Handle data urls before we send it off to the http stack
    if (m_request->url().SchemeIs("data"))
        return handleDataURL(m_request->url());

    if (m_request->url().SchemeIs("browser"))
        return handleBrowserURL(m_request->url());

    URLRequestContext* context = WebRequestContext::get(isPrivateBrowsing);
    m_request->set_context(context);

    m_request->Start();
}

void WebRequest::cancel()
{
    ASSERT(m_loadState >= Started, "Cancel called on a not started WebRequest: (%s)", m_url.c_str());
    ASSERT(m_loadState != Cancelled, "Cancel called on an already cancelled WebRequest: (%s)", m_url.c_str());

    // There is a possible race condition between the IO thread finishing the request and
    // the WebCore thread cancelling it. If the request has already finished, do
    // nothing to avoid sending duplicate finish messages to WebCore.
    if (m_loadState > Cancelled) {
        return;
    }
    ASSERT(m_request, "Request set to 0 before it is finished");

    m_loadState = Cancelled;

    m_request->Cancel();
    finish(true);
}

void WebRequest::handleAndroidURL()
{
    JNIEnv* env = JSC::Bindings::getJNIEnv();
    if (m_inputStream == 0) {
        m_loadState = Finished;
        OwnPtr<WebResponse> webResponse(new WebResponse(m_url, "", 0, "", 0));
        m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
                m_urlLoader.get(), &WebUrlLoaderClient::didFail, webResponse.release()));
        return;
    }

    m_loadState = Response;

    // Get the MIME type from the URL. "text/html" is a last resort, hopefully overridden.
    std::string mimeType("text/html");

    // Gmail appends the MIME to the end of the URL, with a ? separator.
    size_t mimeTypeIndex = m_url.find_last_of('?');
    if (mimeTypeIndex != std::string::npos) {
        mimeType.assign(m_url.begin() + mimeTypeIndex + 1, m_url.end());
    } else {
        // Get the MIME type from the file extension, if any.
        FilePath path(m_url);
        net::GetMimeTypeFromFile(path, &mimeType);
    }

    OwnPtr<WebResponse> webResponse(new WebResponse(m_url, mimeType, 0, "", 200));
    m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
            m_urlLoader.get(), &WebUrlLoaderClient::didReceiveResponse, webResponse.release()));

    int size = 0;
    jclass bridgeClass = env->FindClass("android/webkit/BrowserFrame");
    jmethodID method = env->GetStaticMethodID(bridgeClass, "readFromStream", "(Ljava/io/InputStream;[B)I");

    jbyteArray jb = env->NewByteArray(kInitialReadBufSize);
    do {
        size = (int)env->CallStaticIntMethod(bridgeClass, method, m_inputStream, jb);
        if (size < 0) // -1 is EOF
            break;

        // data is deleted in WebUrlLoaderClient::didReceiveAndroidFileData
        // data is sent to the webcore thread
        OwnPtr<std::vector<char> > data(new std::vector<char>(size));
        env->GetByteArrayRegion(jb, 0, size, (jbyte*)&data->front());

        m_loadState = GotData;
        m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
                m_urlLoader.get(), &WebUrlLoaderClient::didReceiveAndroidFileData, data.release()));
    } while (true);

    env->DeleteLocalRef(jb);
    env->DeleteLocalRef(bridgeClass);

    finish(true);
}

void WebRequest::handleDataURL(GURL url)
{
    OwnPtr<std::string> data(new std::string);
    std::string mimeType;
    std::string charset;

    if (net::DataURL::Parse(url, &mimeType, &charset, data.get())) {
        // PopulateURLResponse from chrome implementation
        // weburlloader_impl.cc
        m_loadState = Response;
        OwnPtr<WebResponse> webResponse(new WebResponse(url.spec(), mimeType, data->size(), charset, 200));
        m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
                m_urlLoader.get(), &WebUrlLoaderClient::didReceiveResponse, webResponse.release()));

        if (!data->empty()) {
            m_loadState = GotData;
            m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
                    m_urlLoader.get(), &WebUrlLoaderClient::didReceiveDataUrl, data.release()));
        }
    } else {
        // handle the failed case
    }

    finish(true);
}

void WebRequest::handleBrowserURL(GURL url)
{
    std::string data("data:text/html;charset=utf-8,");
    if (url.spec() == "browser:incognito") {
        AssetManager* assetManager = globalAssetManager();
        Asset* asset = assetManager->open("webkit/incognito_mode_start_page.html", Asset::ACCESS_BUFFER);
        if (asset) {
            data.append((const char*)asset->getBuffer(false), asset->getLength());
            delete asset;
        }
    }
    GURL dataURL(data.c_str());
    handleDataURL(dataURL);
}

// Called upon a server-initiated redirect.  The delegate may call the
// request's Cancel method to prevent the redirect from being followed.
// Since there may be multiple chained redirects, there may also be more
// than one redirect call.
//
// When this function is called, the request will still contain the
// original URL, the destination of the redirect is provided in 'new_url'.
// If the delegate does not cancel the request and |*defer_redirect| is
// false, then the redirect will be followed, and the request's URL will be
// changed to the new URL.  Otherwise if the delegate does not cancel the
// request and |*defer_redirect| is true, then the redirect will be
// followed once FollowDeferredRedirect is called on the URLRequest.
//
// The caller must set |*defer_redirect| to false, so that delegates do not
// need to set it if they are happy with the default behavior of not
// deferring redirect.
void WebRequest::OnReceivedRedirect(URLRequest* newRequest, const GURL& newUrl, bool* deferRedirect)
{
    ASSERT(m_loadState < Response, "Redirect after receiving response");

    if (newRequest && newRequest->status().is_success()) {
        OwnPtr<WebResponse> webResponse(new WebResponse(newRequest));
        webResponse->setUrl(newUrl.spec());
        m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
                m_urlLoader.get(), &WebUrlLoaderClient::willSendRequest, webResponse.release()));
    } else {
        // why would this happen? And what to do?
    }

    // Here we should check if the url we get back from webkit is the same
    // as newUrl, but since we are on a different thread that is not
    // possible. Look into later.
    return;
}

// Called when we receive an authentication failure.  The delegate should
// call request->SetAuth() with the user's credentials once it obtains them,
// or request->CancelAuth() to cancel the login and display the error page.
// When it does so, the request will be reissued, restarting the sequence
// of On* callbacks.
void WebRequest::OnAuthRequired(URLRequest* request, net::AuthChallengeInfo* authInfo)
{
    ASSERT(m_loadState == Started, "OnAuthRequired called on a WebRequest not in STARTED state (state=%d)", m_loadState);

    scoped_refptr<net::AuthChallengeInfo> authInfoPtr(authInfo);
    m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
            m_urlLoader.get(), &WebUrlLoaderClient::authRequired, authInfoPtr));
}

// After calling Start(), the delegate will receive an OnResponseStarted
// callback when the request has completed.  If an error occurred, the
// request->status() will be set.  On success, all redirects have been
// followed and the final response is beginning to arrive.  At this point,
// meta data about the response is available, including for example HTTP
// response headers if this is a request for a HTTP resource.
void WebRequest::OnResponseStarted(URLRequest* request)
{
    ASSERT(m_loadState == Started, "Got response after receiving response");

    m_loadState = Response;
    if (request && request->status().is_success()) {
        OwnPtr<WebResponse> webResponse(new WebResponse(request));
        m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
                m_urlLoader.get(), &WebUrlLoaderClient::didReceiveResponse, webResponse.release()));

        // Start reading the response
        startReading();
    } else {
        finish(false);
    }
}

void WebRequest::setAuth(const string16& username, const string16& password)
{
    ASSERT(m_loadState == Started, "setAuth called on a WebRequest not in STARTED state (state=%d)", m_loadState);

    m_request->SetAuth(username, password);
}

void WebRequest::cancelAuth()
{
    ASSERT(m_loadState == Started, "cancelAuth called on a WebRequest not in STARTED state (state=%d)", m_loadState);

    m_request->CancelAuth();
}

void WebRequest::startReading()
{
    ASSERT(m_loadState == Response || m_loadState == GotData, "StartReading in state other than RESPONSE and GOTDATA");

    int bytesRead = 0;

    // chrome only does one read, and schedules the next on the same thread
    while (true) {
        if (read(&bytesRead)) {
            // bytesRead == 0 indicates finished
            if (!bytesRead) {
                finish(true);
                break;
            }

            m_loadState = GotData;
            // Read ok, forward buffer to webcore
            m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
                    m_urlLoader.get(), &WebUrlLoaderClient::didReceiveData, m_networkBuffer, bytesRead));
            m_networkBuffer = 0;
        } else if (m_request && m_request->status().is_io_pending()) {
            // got io_pending, so break and wait for read
            break;
        } else {
            // Error, stop and send back
            finish(false);
            break;
        }
    }
}

bool WebRequest::read(int* bytesRead)
{
    ASSERT(m_loadState == Response || m_loadState == GotData, "read in state other than RESPONSE and GOTDATA");
    ASSERT(m_networkBuffer == 0, "Read called with a nonzero buffer");

    // TODO: when asserts work, check that the buffer is 0 here
    m_networkBuffer = new net::IOBuffer(kInitialReadBufSize);
    return m_request->Read(m_networkBuffer, kInitialReadBufSize, bytesRead);
}

// This is called when there is data available

// Called when the a Read of the response body is completed after an
// IO_PENDING status from a Read() call.
// The data read is filled into the buffer which the caller passed
// to Read() previously.
//
// If an error occurred, request->status() will contain the error,
// and bytes read will be -1.
void WebRequest::OnReadCompleted(URLRequest* request, int bytesRead)
{
    ASSERT(m_loadState == Response || m_loadState == GotData, "OnReadCompleted in state other than RESPONSE and GOTDATA");

    if (request->status().is_success()) {
        m_loadState = GotData;
        m_urlLoader->maybeCallOnMainThread(NewRunnableMethod(
                m_urlLoader.get(), &WebUrlLoaderClient::didReceiveData, m_networkBuffer, bytesRead));
        m_networkBuffer = 0;

        // Get the rest of the data
        startReading();
    } else {
        finish(false);
    }
}

} // namespace android
