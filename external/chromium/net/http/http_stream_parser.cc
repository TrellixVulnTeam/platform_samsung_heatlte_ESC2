// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/http_stream_parser.h"
//#include "SecProductFeature_RIL.h"
#include "base/compiler_specific.h"
#include "base/metrics/histogram.h"
#include "base/string_util.h"
#include "net/base/address_list.h"
#include "net/base/auth.h"
#include "net/base/io_buffer.h"
#include "net/base/ssl_cert_request_info.h"
#include "net/http/http_net_log_params.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_request_info.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/client_socket_handle.h"

#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING
//< RNTFIX:: Smart Bonding
#include "net/http/multirat_watchdogthread.h"
#include "net/base/address_list.h"
#include "net/http/multirat_multisocket.h"
#include "net/http/multirat_conf.h"
#include "android/jni/multirat_jni.h"
#include <arpa/inet.h>
#endif
//> RNTFIX

namespace net {

HttpStreamParser::HttpStreamParser(ClientSocketHandle* connection,
                                   const HttpRequestInfo* request,
                                   GrowableIOBuffer* read_buffer,
                                   const BoundNetLog& net_log)
    : io_state_(STATE_NONE),
      request_(request),
      request_headers_(NULL),
      request_body_(NULL),
      read_buf_(read_buffer),
      read_buf_unused_offset_(0),
      response_header_start_offset_(-1),
      response_body_length_(-1),
      response_body_read_(0),
      chunked_decoder_(NULL),
      user_read_buf_(NULL),
      user_read_buf_len_(0),
      user_callback_(NULL),
      connection_(connection),
      net_log_(net_log),
      ALLOW_THIS_IN_INITIALIZER_LIST(
          io_callback_(this, &HttpStreamParser::OnIOComplete)),
      chunk_length_(0),
      chunk_length_without_encoding_(0),
//< RNTFIX:: Smart Bonding
#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING
      bEnablingMultiRat(false),
      pMultiSockStream(NULL),
      pMulRatWatchDog(NULL),
      bMultiRatCheckDone(false),
      resp_conn_keepalive(false),
      IsMultiSocketDataBufferReady(false),
      IsMultiSocketThreadStarted(false),
      totalExpectedBytes(0),
      instanceSize(0),
      sbStartedFlag(false),
      sbThreadId(-1),
#endif
//> RNTFIX
      sent_last_chunk_(false) {
  DCHECK_EQ(0, read_buffer->offset());
#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING
  logObj = multirat_configuration::getInstance()->getLogObj();
#endif
}

HttpStreamParser::~HttpStreamParser() 
{
//< RNTFIX:: Smart Bonding
#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING
  if(logObj)logObj->write("MAIN-THREAD : Parser destructor started");
  if(NULL != pMultiSockStream)
  {
    delete pMultiSockStream;
    pMultiSockStream = NULL;
  }
  if(NULL != pMulRatWatchDog)
  {
    if(pMulRatWatchDog->getThreadStatus() == THREAD_INIT){usleep(100000);}
    delete pMulRatWatchDog;
    pMulRatWatchDog = NULL;
  }
  if(printLog)LOG(ERROR) << "Http Stream Parser destructor called";
  
  // Release smart Bonding
  if (sbStartedFlag) 
  {
    if(IsResponseBodyComplete())
    {
       android::multirat_feature::StopSBUsageWithReason(sbThreadId,STOPSB_ERRCODE_SUCCESS); 
    }
    else
    {
       android::multirat_feature::StopSBUsageWithReason(sbThreadId,STOPSB_ERRCODE_FAILED);
    }
  }
  sbStartedFlag = false;
  if(logObj)logObj->write("MAIN-THREAD : Parser destructor ended for sesssion %lld\n\n",response_body_length_);
#endif
//> RNTFIX
  if (request_body_ != NULL && request_body_->is_chunked())
    request_body_->set_chunk_callback(NULL);
}

int HttpStreamParser::SendRequest(const std::string& request_line,
                                  const HttpRequestHeaders& headers,
                                  UploadDataStream* request_body,
                                  HttpResponseInfo* response,
                                  CompletionCallback* callback) {
  DCHECK_EQ(STATE_NONE, io_state_);
  DCHECK(!user_callback_);
  DCHECK(callback);
  DCHECK(response);

  if (net_log_.IsLoggingAllEvents()) {
    net_log_.AddEvent(
        NetLog::TYPE_HTTP_TRANSACTION_SEND_REQUEST_HEADERS,
        make_scoped_refptr(new NetLogHttpRequestParameter(
            request_line, headers)));
  }
  response_ = response;

  // Put the peer's IP address and port into the response.
  AddressList address;
  int result = connection_->socket()->GetPeerAddress(&address);
  if (result != OK)
    return result;
  response_->socket_address = HostPortPair::FromAddrInfo(address.head());

  std::string request = request_line + headers.ToString();
//< RNTFIX:: Smart Bonding
#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING
  connection_->socket()->GetPeerAddress(&serv_addr); 
  requestUrl = request_line;
  requestHeader = headers;
#endif
//> RNTFIX
  scoped_refptr<StringIOBuffer> headers_io_buf(new StringIOBuffer(request));
  request_headers_ = new DrainableIOBuffer(headers_io_buf,
                                           headers_io_buf->size());
  request_body_.reset(request_body);
  if (request_body_ != NULL && request_body_->is_chunked()) {
    request_body_->set_chunk_callback(this);
    const int kChunkHeaderFooterSize = 12;  // 2 CRLFs + max of 8 hex chars.
    chunk_buf_ = new IOBuffer(request_body_->GetMaxBufferSize() +
                              kChunkHeaderFooterSize);
  }

  io_state_ = STATE_SENDING_HEADERS;
  result = DoLoop(OK);
  if (result == ERR_IO_PENDING)
    user_callback_ = callback;

  return result > 0 ? OK : result;
}

int HttpStreamParser::ReadResponseHeaders(CompletionCallback* callback) {
  DCHECK(io_state_ == STATE_REQUEST_SENT || io_state_ == STATE_DONE);
  DCHECK(!user_callback_);
  DCHECK(callback);

  // This function can be called with io_state_ == STATE_DONE if the
  // connection is closed after seeing just a 1xx response code.
  if (io_state_ == STATE_DONE)
    return ERR_CONNECTION_CLOSED;

  int result = OK;
  io_state_ = STATE_READ_HEADERS;

  if (read_buf_->offset() > 0) {
    // Simulate the state where the data was just read from the socket.
    result = read_buf_->offset() - read_buf_unused_offset_;
    read_buf_->set_offset(read_buf_unused_offset_);
  }
  if (result > 0)
    io_state_ = STATE_READ_HEADERS_COMPLETE;

  result = DoLoop(result);
  if (result == ERR_IO_PENDING)
    user_callback_ = callback;

  return result > 0 ? OK : result;
}

void HttpStreamParser::Close(bool not_reusable) 
{
//< RNTFIX:: Smart Bonding
#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING
  if(printLog)LOG(ERROR) << "Http Stream Parser socket close started";
  if(logObj)logObj->write("MAIN-THREAD : Http Stream Parser socket close started");
  if(!IsMultiSocketDataBufferReady)
  {
    if (not_reusable && connection_->socket())
      connection_->socket()->Disconnect();
    connection_->Reset();
  }
  if(NULL != pMultiSockStream)
  {
    delete pMultiSockStream;
    pMultiSockStream = NULL;
  }
  if(NULL != pMulRatWatchDog)
  {
    if(pMulRatWatchDog->getThreadStatus() == THREAD_INIT){usleep(100000);}
    delete pMulRatWatchDog;
    pMulRatWatchDog = NULL;
  }
  if (sbStartedFlag)
  {
    if(IsResponseBodyComplete())
    {
       android::multirat_feature::StopSBUsageWithReason(sbThreadId,STOPSB_ERRCODE_SUCCESS);
    }
    else
    {
       android::multirat_feature::StopSBUsageWithReason(sbThreadId,STOPSB_ERRCODE_FAILED);
    }
  }
  sbStartedFlag = false;
  if(printLog)LOG(ERROR) << "Http Stream Parser socket close Ended";
  if(logObj)logObj->write("MAIN-THREAD : Http Stream Parser socket close Ended");
#else
//> RNTFIX
  if (not_reusable && connection_->socket())
    connection_->socket()->Disconnect();
  connection_->Reset();
#endif
}

int HttpStreamParser::ReadResponseBody(IOBuffer* buf, int buf_len,
                                       CompletionCallback* callback) {
  DCHECK(io_state_ == STATE_BODY_PENDING || io_state_ == STATE_DONE);
  DCHECK(!user_callback_);
  DCHECK(callback);
  DCHECK_LE(buf_len, kMaxBufSize);

  if (io_state_ == STATE_DONE)
    return OK;

  user_read_buf_ = buf;
  user_read_buf_len_ = buf_len;
  io_state_ = STATE_READ_BODY;

  int result = DoLoop(OK);
  if (result == ERR_IO_PENDING)
    user_callback_ = callback;

  return result;
}

void HttpStreamParser::OnIOComplete(int result) {
  result = DoLoop(result);

  // The client callback can do anything, including destroying this class,
  // so any pending callback must be issued after everything else is done.
  if (result != ERR_IO_PENDING && user_callback_) {
    CompletionCallback* c = user_callback_;
    user_callback_ = NULL;
    c->Run(result);
  }
}

void HttpStreamParser::OnChunkAvailable() {
  // This method may get called while sending the headers or body, so check
  // before processing the new data. If we were still initializing or sending
  // headers, we will automatically start reading the chunks once we get into
  // STATE_SENDING_BODY so nothing to do here.
  DCHECK(io_state_ == STATE_SENDING_HEADERS || io_state_ == STATE_SENDING_BODY);
  if (io_state_ == STATE_SENDING_BODY)
    OnIOComplete(0);
}

int HttpStreamParser::DoLoop(int result) {
  bool can_do_more = true;
  do {
    switch (io_state_) {
      case STATE_SENDING_HEADERS:
        if (result < 0)
          can_do_more = false;
        else
          result = DoSendHeaders(result);
        break;
      case STATE_SENDING_BODY:
        if (result < 0)
          can_do_more = false;
        else
          result = DoSendBody(result);
        break;
      case STATE_REQUEST_SENT:
        DCHECK(result != ERR_IO_PENDING);
        can_do_more = false;
        break;
      case STATE_READ_HEADERS:
        net_log_.BeginEvent(NetLog::TYPE_HTTP_STREAM_PARSER_READ_HEADERS, NULL);
        result = DoReadHeaders();
        break;
      case STATE_READ_HEADERS_COMPLETE:
        result = DoReadHeadersComplete(result);
        net_log_.EndEventWithNetErrorCode(
            NetLog::TYPE_HTTP_STREAM_PARSER_READ_HEADERS, result);
        break;
      case STATE_BODY_PENDING:
        DCHECK(result != ERR_IO_PENDING);
        can_do_more = false;
        break;
      case STATE_READ_BODY:
        result = DoReadBody();
        // DoReadBodyComplete handles error conditions.
        break;
      case STATE_READ_BODY_COMPLETE:
        result = DoReadBodyComplete(result);
        break;
      case STATE_DONE:
        DCHECK(result != ERR_IO_PENDING);
        can_do_more = false;
        break;
      default:
        NOTREACHED();
        can_do_more = false;
        break;
    }
  } while (result != ERR_IO_PENDING && can_do_more);

  return result;
}

int HttpStreamParser::DoSendHeaders(int result) {
  request_headers_->DidConsume(result);
  int bytes_remaining = request_headers_->BytesRemaining();
  if (bytes_remaining > 0) {
    // Record our best estimate of the 'request time' as the time when we send
    // out the first bytes of the request headers.
    if (bytes_remaining == request_headers_->size()) {
      response_->request_time = base::Time::Now();

      // We'll record the count of uncoalesced packets IFF coalescing will help,
      // and otherwise we'll use an enum to tell why it won't help.
      enum COALESCE_POTENTIAL {
        // Coalescing won't reduce packet count.
        NO_ADVANTAGE = 0,
        // There is only a header packet or we have a request body but the
        // request body isn't available yet (can't coalesce).
        HEADER_ONLY = 1,
        // Various cases of coalasced savings.
        COALESCE_POTENTIAL_MAX = 30
      };
      size_t coalesce = HEADER_ONLY;
      if (request_body_ != NULL && !request_body_->is_chunked()) {
        const size_t kBytesPerPacket = 1430;
        uint64 body_packets = (request_body_->size() + kBytesPerPacket - 1) /
                              kBytesPerPacket;
        uint64 header_packets = (bytes_remaining + kBytesPerPacket - 1) /
                                kBytesPerPacket;
        uint64 coalesced_packets = (request_body_->size() + bytes_remaining +
                                    kBytesPerPacket - 1) / kBytesPerPacket;
        if (coalesced_packets < header_packets + body_packets) {
          if (coalesced_packets > COALESCE_POTENTIAL_MAX)
            coalesce = COALESCE_POTENTIAL_MAX;
          else
            coalesce = static_cast<size_t>(header_packets + body_packets);
        } else {
          coalesce = NO_ADVANTAGE;
        }
      }
      UMA_HISTOGRAM_ENUMERATION("Net.CoalescePotential", coalesce,
                                COALESCE_POTENTIAL_MAX);
    }
    result = connection_->socket()->Write(request_headers_,
                                          bytes_remaining,
                                          &io_callback_);
  } else if (request_body_ != NULL &&
             (request_body_->is_chunked() || request_body_->size())) {
    io_state_ = STATE_SENDING_BODY;
    result = OK;
  } else {
    io_state_ = STATE_REQUEST_SENT;
  }
  return result;
}

int HttpStreamParser::DoSendBody(int result) {
  if (request_body_->is_chunked()) {
    chunk_length_ -= result;
    if (chunk_length_) {
      memmove(chunk_buf_->data(), chunk_buf_->data() + result, chunk_length_);
      return connection_->socket()->Write(chunk_buf_, chunk_length_,
                                          &io_callback_);
    }

    if (sent_last_chunk_) {
      io_state_ = STATE_REQUEST_SENT;
      return OK;
    }

    request_body_->MarkConsumedAndFillBuffer(chunk_length_without_encoding_);
    chunk_length_without_encoding_ = 0;
    chunk_length_ = 0;

    int buf_len = static_cast<int>(request_body_->buf_len());
    if (request_body_->eof()) {
      static const char kLastChunk[] = "0\r\n\r\n";
      chunk_length_ = strlen(kLastChunk);
      memcpy(chunk_buf_->data(), kLastChunk, chunk_length_);
      sent_last_chunk_ = true;
    } else if (buf_len) {
      // Encode and send the buffer as 1 chunk.
      std::string chunk_header = StringPrintf("%X\r\n", buf_len);
      char* chunk_ptr = chunk_buf_->data();
      memcpy(chunk_ptr, chunk_header.data(), chunk_header.length());
      chunk_ptr += chunk_header.length();
      memcpy(chunk_ptr, request_body_->buf()->data(), buf_len);
      chunk_ptr += buf_len;
      memcpy(chunk_ptr, "\r\n", 2);
      chunk_length_without_encoding_ = buf_len;
      chunk_length_ = chunk_header.length() + buf_len + 2;
    }

    if (!chunk_length_)  // More POST data is yet to come?
      return ERR_IO_PENDING;

    return connection_->socket()->Write(chunk_buf_, chunk_length_,
                                        &io_callback_);
  }

  // Non-chunked request body.
  request_body_->MarkConsumedAndFillBuffer(result);

  if (!request_body_->eof()) {
    int buf_len = static_cast<int>(request_body_->buf_len());
    result = connection_->socket()->Write(request_body_->buf(), buf_len,
                                          &io_callback_);
  } else {
    io_state_ = STATE_REQUEST_SENT;
  }
  return result;
}

int HttpStreamParser::DoReadHeaders() {
  io_state_ = STATE_READ_HEADERS_COMPLETE;

  // Grow the read buffer if necessary.
  if (read_buf_->RemainingCapacity() == 0)
    read_buf_->SetCapacity(read_buf_->capacity() + kHeaderBufInitialSize);

  // http://crbug.com/16371: We're seeing |user_buf_->data()| return NULL.
  // See if the user is passing in an IOBuffer with a NULL |data_|.
  CHECK(read_buf_->data());

  return connection_->socket()->Read(read_buf_,
                                     read_buf_->RemainingCapacity(),
                                     &io_callback_);
}

int HttpStreamParser::DoReadHeadersComplete(int result) {
  if (result == 0)
    result = ERR_CONNECTION_CLOSED;

  if (result < 0 && result != ERR_CONNECTION_CLOSED) {
    io_state_ = STATE_DONE;
    return result;
  }
  // If we've used the connection before, then we know it is not a HTTP/0.9
  // response and return ERR_CONNECTION_CLOSED.
  if (result == ERR_CONNECTION_CLOSED && read_buf_->offset() == 0 &&
      connection_->is_reused()) {
    io_state_ = STATE_DONE;
    return result;
  }

  // Record our best estimate of the 'response time' as the time when we read
  // the first bytes of the response headers.
  if (read_buf_->offset() == 0 && result != ERR_CONNECTION_CLOSED)
    response_->response_time = base::Time::Now();

  if (result == ERR_CONNECTION_CLOSED) {
    // The connection closed before we detected the end of the headers.
    // parse things as well as we can and let the caller decide what to do.
    if (read_buf_->offset() == 0) {
      // The connection was closed before any data was sent. Likely an error
      // rather than empty HTTP/0.9 response.
      io_state_ = STATE_DONE;
      return ERR_EMPTY_RESPONSE;
    } else {
      int end_offset;
      if (response_header_start_offset_ >= 0) {
        io_state_ = STATE_READ_BODY_COMPLETE;
        end_offset = read_buf_->offset();
      } else {
        io_state_ = STATE_BODY_PENDING;
        end_offset = 0;
      }
      int rv = DoParseResponseHeaders(end_offset);
      if (rv < 0)
        return rv;
      return result;
    }
  }

  read_buf_->set_offset(read_buf_->offset() + result);
  DCHECK_LE(read_buf_->offset(), read_buf_->capacity());
  DCHECK_GE(result,  0);

  int end_of_header_offset = ParseResponseHeaders();

  // Note: -1 is special, it indicates we haven't found the end of headers.
  // Anything less than -1 is a net::Error, so we bail out.
  if (end_of_header_offset < -1)
    return end_of_header_offset;

  if (end_of_header_offset == -1) {
    io_state_ = STATE_READ_HEADERS;
    // Prevent growing the headers buffer indefinitely.
    if (read_buf_->offset() - read_buf_unused_offset_ >= kMaxHeaderBufSize) {
      io_state_ = STATE_DONE;
      return ERR_RESPONSE_HEADERS_TOO_BIG;
    }
  } else {
    // Note where the headers stop.
    read_buf_unused_offset_ = end_of_header_offset;
//< RNTFIX:: Smart Bonding
#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING
    resp_conn_keepalive = response_->headers->IsKeepAlive();
    int64 range_start, range_end, object_size;
    if((response_->headers->response_code() == 206) && (response_->headers->GetContentRange(&range_start, &range_end, &object_size)))
    {
      instanceSize = object_size; 
    }
#endif
//>
    if (response_->headers->response_code() / 100 == 1) {
      // After processing a 1xx response, the caller will ask for the next
      // header, so reset state to support that.  We don't just skip these
      // completely because 1xx codes aren't acceptable when establishing a
      // tunnel.
      io_state_ = STATE_REQUEST_SENT;
      response_header_start_offset_ = -1;
    } else {
      io_state_ = STATE_BODY_PENDING;
      CalculateResponseBodySize();
      // If the body is 0, the caller may not call ReadResponseBody, which
      // is where any extra data is copied to read_buf_, so we move the
      // data here and transition to DONE.
      if (response_body_length_ == 0) {
        io_state_ = STATE_DONE;
        int extra_bytes = read_buf_->offset() - read_buf_unused_offset_;
        if (extra_bytes) {
          CHECK_GT(extra_bytes, 0);
          memmove(read_buf_->StartOfBuffer(),
                  read_buf_->StartOfBuffer() + read_buf_unused_offset_,
                  extra_bytes);
        }
        read_buf_->SetCapacity(extra_bytes);
        read_buf_unused_offset_ = 0;
        return OK;
      }
    }
  }
  return result;
}

//< RNTFIX:: Smart Bonding
#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING

bool HttpStreamParser::getRequestHeaderValue(const char* headerName, std::string *val)
{
  return requestHeader.GetHeader(headerName, val);
}

bool HttpStreamParser::shouldEnableMultiRat()
{
  multirat_configuration* ptrConf = multirat_configuration::getInstance();
  bool bUseMultiRat = ( ((request_->method == "GET") 
                       //  && (response_->headers->GetHttpVersion() >= HttpVersion(1, 1))
                         && (!request_->url.SchemeIs("https")) 
                         && ((response_->headers->response_code() == 200) || (response_->headers->response_code() == 206))
                         && (!(response_->headers->HasHeaderValue("Accept-Ranges","none")))
                         && (response_body_length_ > ptrConf->getMULTIRAT_THRESHOLD())
                        ));
						
  if(bUseMultiRat)
  {
    if(printLog)LOG(ERROR) << "Multi Rat Enable for response_body_length_ " << response_body_length_;
    if(logObj)logObj->write("MAIN-THREAD : Watch dog started for length %lld", response_body_length_);
  }

  bMultiRatCheckDone = true;
  return bUseMultiRat;
}

bool HttpStreamParser::isHeaderExist(std::string headername, std::string & conn_hdr_val)
{
  if(printLog)LOG(ERROR) << " isConnectionHeaderExist headername " << headername;
  return requestHeader.GetHeader(headername, &conn_hdr_val);
}

void HttpStreamParser::setHeader(std::string headername, std::string conn_hdr_val)
{
  if(printLog)LOG(ERROR)<<" setHeader headername "<<headername<<" setHeader headervalue "<< conn_hdr_val;
  requestHeader.SetHeader(headername, conn_hdr_val);
}

bool HttpStreamParser::isRangeRequest()
{
  std::string range_header;
  return  requestHeader.GetHeader( net::HttpRequestHeaders::kRange, &range_header);
}

bool HttpStreamParser::isNetworkServerAddress()
{
  char ipstr[INET_ADDRSTRLEN] = {0};
  struct sockaddr_in sa;
  const struct addrinfo* servAddr = serv_addr.head();  

  if(servAddr->ai_addr->sa_family ==  AF_INET )
  {
    if(inet_ntop(AF_INET, &(((sockaddr_in*)(servAddr->ai_addr))->sin_addr), ipstr, INET_ADDRSTRLEN) == NULL)
    {
      if(printLog)LOG(ERROR) << " invalid ipv4 address " << ipstr;
      return false;
    }
    unsigned long d_addr = ((sockaddr_in*)(servAddr->ai_addr))->sin_addr.s_addr;

    bool ismulticast = ((ntohl(d_addr) & 0xf0000000) == 0xe0000000) ? true : false;// 224- 239

    bool isloopback = ((ntohl(d_addr) & 0xff000000) == 0x7f000000) ? true : false;// 127.0.0.0

    bool islinklocal = (((ntohl(d_addr) & 0xffffffff) == 0x00000000) || 
                        ((ntohl(d_addr) & 0xffff0000) == 0xa9fe0000)) ? true: false;// 0.0.0.0 and  169.254
    bool issitelocal = (((ntohl(d_addr) & 0xff000000) == 0x0a000000) || 
                        ((ntohl(d_addr) & 0xffff0000) == 0xc0a80000) || 
                        ((ntohl(d_addr) & 0xfff00000) == 0xac100000)) ? true: false;// 10.0.0.0 - 10.255.255.255 , 192.168 , 172.16 - 172.31

    if(ismulticast || isloopback || islinklocal || issitelocal)
    {
      if(printLog)LOG(ERROR) << " link local or site local or multicast ip address " << ipstr;
      if(printLog)LOG(ERROR) << " multicast " << ismulticast;
      if(printLog)LOG(ERROR) << " isloopback " << isloopback;
      if(printLog)LOG(ERROR) << " islinklocal " << islinklocal;
      if(printLog)LOG(ERROR) << " issitelocal " << issitelocal;
      return false;
    }
    return true;
  }
  else
  {
    if(printLog)LOG(ERROR) << " ipv6 address ";
    return false;	
  }
}

void  HttpStreamParser::removeRangeHeader()
{
  requestHeader.RemoveHeader(net::HttpRequestHeaders::kRange);
}

bool HttpStreamParser::getBytesofRangeRequest(int64 &start )
{
  std::string range_header;
  requestHeader.GetHeader(net::HttpRequestHeaders::kRange, &range_header);
  if(printLog)LOG(ERROR) << "Range header value" <<range_header;

  if(range_header.compare(0,5, "bytes") == 0)
  {
    unsigned int i1 =  range_header.find_first_of("=");
    unsigned int i2 = range_header.find_first_of("-");
    unsigned int i3 = range_header.find_last_of("-");
    unsigned int i4 = range_header.find(",");
    if(i1 ==std::string::npos || i2 ==std::string::npos || i1+1 >= i2 || i2 != i3 || i4 !=std::string::npos) 
    {
      return false;
    }
    else
    {
      std::string sOffset = range_header.substr (i1 + 1, i2 - (i1+1));
      if(!sOffset.empty()) 
      {
        start = atoll(sOffset.c_str());
        return true;
      }
    }
  }
  return false;
}

std::string HttpStreamParser::getRequestHeader()
{
  std::string header = requestUrl + requestHeader.ToString() ;	
  return header;
}

int HttpStreamParser::IsMultiRatRead()
{
  if(bEnablingMultiRat)
  {
    if(!IsMultiSocketDataBufferReady && IsMultiSocketThreadStarted)
    {
    // Read First range request
      if(NULL != pMulRatWatchDog)
      {
        delete pMulRatWatchDog; // delete it now as Socket thread started
        pMulRatWatchDog = NULL;
      }

      if(response_body_read_ >= totalExpectedBytes )
      {
        int errNum = STOPSB_ERRCODE_RRFAILED;
        if(logObj)logObj->write("MAIN-THREAD : Completed Reading Expected bytes from main socket %d",totalExpectedBytes);
        int retval = pMultiSockStream->getFirstRangeStatus(&errNum);
        if(retval == FIRST_RSP_STATUS_PENDING)  // PENDING
        {
          if(-1 == pMultiSockStream->setCallBack(user_read_buf_->data(), user_read_buf_len_,&io_callback_,this))
          {
            return -1;
          }
          else
          {
            IsMultiSocketDataBufferReady = false;
            IsMultiSocketThreadStarted = false;
          }
        }
        else if(retval == FIRST_RSP_STATUS_SUCCESS) // Succcess
        {
          IsMultiSocketDataBufferReady = true;
          connection_->socket()->Disconnect(); // DISCONNECT MAIN SOCKET
          connection_->Reset();
        }
        else
        {
          if(sbStartedFlag)
          {
            android::multirat_feature::StopSBUsageWithReason(sbThreadId,errNum);
            sbStartedFlag = false;
          }

          IsMultiSocketDataBufferReady = false;
          IsMultiSocketThreadStarted = false;
        }
      }
    }
  }
  if(IsMultiSocketDataBufferReady)
  {
    return 1;
  }
  else
  {
    return 0;
  }
}

int HttpStreamParser::setMultiSocketStatus(bool status)
{
  if(status)
  {
    IsMultiSocketDataBufferReady = true;
    connection_->socket()->Disconnect(); // DISCONNECT MAIN SOCKET
    connection_->Reset();
  }
  else
  {
    if(sbStartedFlag)
    {
      android::multirat_feature::StopSBUsageWithReason(sbThreadId,pMultiSockStream->getErrNum());
      sbStartedFlag = false;
    }

    IsMultiSocketDataBufferReady = false;
    IsMultiSocketThreadStarted = false;
  }
  return DoReadBody();
}

int HttpStreamParser::DoMultiRatReadBody()
{
  if(NULL != pMultiSockStream)
  {
    return pMultiSockStream->readBuffer(user_read_buf_->data(), user_read_buf_len_,&io_callback_);
  }
  else
  {
    return -1;
  }
}
#endif
//> RNTFIX

int HttpStreamParser::DoReadBody() {
  io_state_ = STATE_READ_BODY_COMPLETE;

  // There may be some data left over from reading the response headers.
  if (read_buf_->offset()) {
    int available = read_buf_->offset() - read_buf_unused_offset_;
    if (available) {
      CHECK_GT(available, 0);
      int bytes_from_buffer = std::min(available, user_read_buf_len_);
      memcpy(user_read_buf_->data(),
             read_buf_->StartOfBuffer() + read_buf_unused_offset_,
             bytes_from_buffer);
      read_buf_unused_offset_ += bytes_from_buffer;
      if (bytes_from_buffer == available) {
        read_buf_->SetCapacity(0);
        read_buf_unused_offset_ = 0;
      }
      return bytes_from_buffer;
    } else {
      read_buf_->SetCapacity(0);
      read_buf_unused_offset_ = 0;
    }
  }

  // Check to see if we're done reading.
  if (IsResponseBodyComplete())
    return 0;

  DCHECK_EQ(0, read_buf_->offset());

//< RNTFIX:: Smart Bonding
#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING
  if(bMultiRatCheckDone == false)
  {
    if((bEnablingMultiRat = shouldEnableMultiRat()))
    {
      if(isNetworkServerAddress())
      {
        pMulRatWatchDog = new MultiRatWatchDogThread(this);
        pMulRatWatchDog->startThread();
      }
    }
  }
  if(IsMultiSocketThreadStarted)		
  {
    if(IsMultiSocketDataBufferReady)
    {
      return DoMultiRatReadBody();
    }
    else
    {
      if(-1 == IsMultiRatRead())
      {
        return -1;
      }
      if(IsMultiSocketDataBufferReady)
      {
        return DoMultiRatReadBody();
      }
      else
      {
        if ((totalExpectedBytes > 0) && response_body_read_ < totalExpectedBytes)
        {
          return connection_->socket()->Read(user_read_buf_, 
			std::min((totalExpectedBytes - (int)response_body_read_), user_read_buf_len_),&io_callback_);
        }
        else 
        {
          return connection_->socket()->Read(user_read_buf_, user_read_buf_len_,&io_callback_);
        }
      }
    }
  }
  else
  {
    if(pMulRatWatchDog != NULL && pMulRatWatchDog->getThreadStatus() == THREAD_FINISH)
    {
      delete pMulRatWatchDog;
      pMulRatWatchDog = NULL;
    }
    return connection_->socket()->Read(user_read_buf_, user_read_buf_len_,&io_callback_);
  }
#else
  return connection_->socket()->Read(user_read_buf_, user_read_buf_len_,&io_callback_);
#endif
//> RNTFIX
}

int HttpStreamParser::DoReadBodyComplete(int result) {
  // If we didn't get a content-length and aren't using a chunked encoding,
  // the only way to signal the end of a stream is to close the connection,
  // so we don't treat that as an error, though in some cases we may not
  // have completely received the resource.

  if (result == 0 && !IsResponseBodyComplete() && CanFindEndOfResponse())
  {
#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING
    if(printLog)LOG(ERROR) << "ERR_CONNECTION_CLOSED returning as result 0";
    if(logObj)logObj->write("MAIN-THREAD : Connection closed Socket Error");
#endif
    result = ERR_CONNECTION_CLOSED;
  }
  // Filter incoming data if appropriate.  FilterBuf may return an error.
  if (result > 0 && chunked_decoder_.get()) {
    result = chunked_decoder_->FilterBuf(user_read_buf_->data(), result);
    if (result == 0 && !chunked_decoder_->reached_eof()) {
      // Don't signal completion of the Read call yet or else it'll look like
      // we received end-of-file.  Wait for more data.
      io_state_ = STATE_READ_BODY;
      return OK;
    }
  }

  if (result > 0)
    response_body_read_ += result;

  if (result <= 0 || IsResponseBodyComplete()) {
#ifdef SEC_PRODUCT_FEATURE_RIL_ENABLE_SMART_BONDING
    if(printLog)LOG(ERROR) << "STATE_DONE result" << result;
    if(logObj)logObj->write("MAIN-THREAD : Download completed");
#endif
    io_state_ = STATE_DONE;

    // Save the overflow data, which can be in two places.  There may be
    // some left over in |user_read_buf_|, plus there may be more
    // in |read_buf_|.  But the part left over in |user_read_buf_| must have
    // come from the |read_buf_|, so there's room to put it back at the
    // start first.
    int additional_save_amount = read_buf_->offset() - read_buf_unused_offset_;
    int save_amount = 0;
    if (chunked_decoder_.get()) {
      save_amount = chunked_decoder_->bytes_after_eof();
    } else if (response_body_length_ >= 0) {
      int64 extra_data_read = response_body_read_ - response_body_length_;
      if (extra_data_read > 0) {
        save_amount = static_cast<int>(extra_data_read);
        if (result > 0)
          result -= save_amount;
      }
    }

    CHECK_LE(save_amount + additional_save_amount, kMaxBufSize);
    if (read_buf_->capacity() < save_amount + additional_save_amount) {
      read_buf_->SetCapacity(save_amount + additional_save_amount);
    }

    if (save_amount) {
      memcpy(read_buf_->StartOfBuffer(), user_read_buf_->data() + result,
             save_amount);
    }
    read_buf_->set_offset(save_amount);
    if (additional_save_amount) {
      memmove(read_buf_->data(),
              read_buf_->StartOfBuffer() + read_buf_unused_offset_,
              additional_save_amount);
      read_buf_->set_offset(save_amount + additional_save_amount);
    }
    read_buf_unused_offset_ = 0;
  } else {
    io_state_ = STATE_BODY_PENDING;
    user_read_buf_ = NULL;
    user_read_buf_len_ = 0;
  }

  return result;
}

int HttpStreamParser::ParseResponseHeaders() {
  int end_offset = -1;

  // Look for the start of the status line, if it hasn't been found yet.
  if (response_header_start_offset_ < 0) {
    response_header_start_offset_ = HttpUtil::LocateStartOfStatusLine(
        read_buf_->StartOfBuffer() + read_buf_unused_offset_,
        read_buf_->offset() - read_buf_unused_offset_);
  }

  if (response_header_start_offset_ >= 0) {
    end_offset = HttpUtil::LocateEndOfHeaders(
        read_buf_->StartOfBuffer() + read_buf_unused_offset_,
        read_buf_->offset() - read_buf_unused_offset_,
        response_header_start_offset_);
  } else if (read_buf_->offset() - read_buf_unused_offset_ >= 8) {
    // Enough data to decide that this is an HTTP/0.9 response.
    // 8 bytes = (4 bytes of junk) + "http".length()
    end_offset = 0;
  }

  if (end_offset == -1)
    return -1;

  int rv = DoParseResponseHeaders(end_offset);
  if (rv < 0)
    return rv;
  return end_offset + read_buf_unused_offset_;
}

int HttpStreamParser::DoParseResponseHeaders(int end_offset) {
  scoped_refptr<HttpResponseHeaders> headers;
  if (response_header_start_offset_ >= 0) {
    headers = new HttpResponseHeaders(HttpUtil::AssembleRawHeaders(
        read_buf_->StartOfBuffer() + read_buf_unused_offset_, end_offset));
  } else {
    // Enough data was read -- there is no status line.
    headers = new HttpResponseHeaders(std::string("HTTP/0.9 200 OK"));
  }

  // Check for multiple Content-Length headers with a Transfer-Encoding header.
  // If they exist, it's a potential response smuggling attack.

  void* it = NULL;
  const std::string content_length_header("Content-Length");
  std::string content_length_value;
  if (!headers->HasHeader("Transfer-Encoding") &&
      headers->EnumerateHeader(
          &it, content_length_header, &content_length_value)) {
    // Ok, there's no Transfer-Encoding header and there's at least one
    // Content-Length header.  Check if there are any more Content-Length
    // headers, and if so, make sure they have the same value.  Otherwise, it's
    // a possible response smuggling attack.
    std::string content_length_value2;
    while (headers->EnumerateHeader(
        &it, content_length_header, &content_length_value2)) {
      if (content_length_value != content_length_value2)
        return ERR_RESPONSE_HEADERS_MULTIPLE_CONTENT_LENGTH;
    }
  }

  response_->headers = headers;
  response_->vary_data.Init(*request_, *response_->headers);
  return OK;
}

void HttpStreamParser::CalculateResponseBodySize() {
  // Figure how to determine EOF:

  // For certain responses, we know the content length is always 0. From
  // RFC 2616 Section 4.3 Message Body:
  //
  // For response messages, whether or not a message-body is included with
  // a message is dependent on both the request method and the response
  // status code (section 6.1.1). All responses to the HEAD request method
  // MUST NOT include a message-body, even though the presence of entity-
  // header fields might lead one to believe they do. All 1xx
  // (informational), 204 (no content), and 304 (not modified) responses
  // MUST NOT include a message-body. All other responses do include a
  // message-body, although it MAY be of zero length.
  switch (response_->headers->response_code()) {
    // Note that 1xx was already handled earlier.
    case 204:  // No Content
    case 205:  // Reset Content
    case 304:  // Not Modified
      response_body_length_ = 0;
      break;
  }
  if (request_->method == "HEAD")
    response_body_length_ = 0;

  if (response_body_length_ == -1) {
    // Ignore spurious chunked responses from HTTP/1.0 servers and
    // proxies. Otherwise "Transfer-Encoding: chunked" trumps
    // "Content-Length: N"
    if (response_->headers->GetHttpVersion() >= HttpVersion(1, 1) &&
        response_->headers->HasHeaderValue("Transfer-Encoding", "chunked")) {
      chunked_decoder_.reset(new HttpChunkedDecoder());
    } else {
      response_body_length_ = response_->headers->GetContentLength();
      // If response_body_length_ is still -1, then we have to wait
      // for the server to close the connection.
    }
  }
}

uint64 HttpStreamParser::GetUploadProgress() const {
  if (!request_body_.get())
    return 0;

  return request_body_->position();
}

HttpResponseInfo* HttpStreamParser::GetResponseInfo() {
  return response_;
}

bool HttpStreamParser::IsResponseBodyComplete() const {
  if (chunked_decoder_.get())
    return chunked_decoder_->reached_eof();
  if (response_body_length_ != -1)
    return response_body_read_ >= response_body_length_;

  return false;  // Must read to EOF.
}

bool HttpStreamParser::CanFindEndOfResponse() const {
  return chunked_decoder_.get() || response_body_length_ >= 0;
}

bool HttpStreamParser::IsMoreDataBuffered() const {
  return read_buf_->offset() > read_buf_unused_offset_;
}

bool HttpStreamParser::IsConnectionReused() const {
  ClientSocketHandle::SocketReuseType reuse_type = connection_->reuse_type();
  return connection_->is_reused() ||
         reuse_type == ClientSocketHandle::UNUSED_IDLE;
}

void HttpStreamParser::SetConnectionReused() {
  connection_->set_is_reused(true);
}

bool HttpStreamParser::IsConnectionReusable() const {
  return connection_->socket() && connection_->socket()->IsConnectedAndIdle();
}

void HttpStreamParser::GetSSLInfo(SSLInfo* ssl_info) {
  if (request_->url.SchemeIs("https") && connection_->socket()) {
    SSLClientSocket* ssl_socket =
        static_cast<SSLClientSocket*>(connection_->socket());
    ssl_socket->GetSSLInfo(ssl_info);
  }
}

void HttpStreamParser::GetSSLCertRequestInfo(
    SSLCertRequestInfo* cert_request_info) {
  if (request_->url.SchemeIs("https") && connection_->socket()) {
    SSLClientSocket* ssl_socket =
        static_cast<SSLClientSocket*>(connection_->socket());
    ssl_socket->GetSSLCertRequestInfo(cert_request_info);
  }
}

}  // namespace net
