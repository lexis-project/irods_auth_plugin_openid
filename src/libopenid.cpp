//#define USE_SSL 1 (libpam.cpp)
#include "sslSockComm.h"

#include "authCheck.h"
#include "authPluginRequest.h"
#include "authRequest.h"
#include "authResponse.h"
#include "authenticate.h"
#include "genQuery.h"
#include "irods_auth_constants.hpp"
#include "irods_auth_plugin.hpp"
#include "irods_openid_object.hpp"
#include "irods_client_server_negotiation.hpp"
#include "irods_configuration_keywords.hpp"
#include "irods_error.hpp"
#include "irods_generic_auth_object.hpp"
#include "irods_kvp_string_parser.hpp"
#include "irods_server_properties.hpp"
#include "irods_stacktrace.hpp"
#include "miscServerFunct.hpp"
#include "rodsErrorTable.h"
#include "rodsLog.h"

#ifdef RODS_SERVER
#include "rsGenQuery.hpp"
#include "rsAuthCheck.hpp"
#include "rsAuthResponse.hpp"
#include "rsAuthRequest.hpp"
#endif

#include <openssl/md5.h>
#include <gssapi.h>
#include <string>

///OPENID includes
//#include <iostream>
#include <sstream>
#include <map>
#include <regex>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <curl/curl.h>
#include <boost/algorithm/string.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/format.hpp>
///END OPENID includes
///DECLARATIONS

#ifdef RODS_SERVER
// =-=-=-=-=-=-=-
// NOTE:: this needs to become a property
// Set requireServerAuth to 1 to fail authentications from
// un-authenticated Servers (for example, if the LocalZoneSID
// is not set)
static const int requireServerAuth = 0;
static int openidAuthReqStatus = 0;
static int openidAuthReqError = 0;
static const int openidAuthErrorSize = 1000;
static char openidAuthReqErrorMsg[openidAuthErrorSize];
#endif

static const std::string AUTH_OPENID_SCHEME("openid");

boost::property_tree::ptree *get_provider_metadata(std::string provider_metadata_url);

void send_success(int sockfd);
std::string *accept_request(int portno);

std::map<std::string,std::string> *get_params(std::string req);

std::string *get_access_token(std::string token_endpoint_url,
                         std::string authorization_code,
                         std::string client_id,
                         std::string client_secret,
                         std::string redirect_uri);

boost::property_tree::ptree *get_provider_metadata(std::string provider_metadata_url);
bool get_provider_metadata_field(std::string provider_metadata_url, const std::string fieldname, std::string& value);

irods::error openid_authorize();
///END DECLARATIONS

irods::error openid_auth_establish_context(
    irods::plugin_context& _ctx ) {
    std::cout << "entering openid_auth_establish_context" << std::endl;
    irods::error result = SUCCESS();
    irods::error ret;

    ret = _ctx.valid<irods::generic_auth_object>();
    if ( !ret.ok()) {
        return ERROR(SYS_INVALID_INPUT_PARAM, "Invalid plugin context.");
    }
    std::cout << "leaving openid_auth_establish_context" << std::endl;
    return ret;
}

irods::error openid_auth_client_start(
    irods::plugin_context& _ctx,
    rcComm_t*              _comm,
    const char*            _inst_name)
{
    std::cout << "entering openid_auth_client_start" << std::endl;
    irods::error result = SUCCESS();
    irods::error ret;

    ret = _ctx.valid<irods::generic_auth_object>();
    if ( ( result = ASSERT_PASS( ret, "Invalid plugin context.") ).ok() ) {
        if ( ( result = ASSERT_ERROR( _comm != NULL, SYS_INVALID_INPUT_PARAM, "Null rcComm_t pointer." ) ).ok() ) {
            irods::generic_auth_object_ptr ptr = boost::dynamic_pointer_cast<irods::generic_auth_object>( _ctx.fco() );
            
            // set the user name from the conn
            ptr->user_name( _comm->proxyUser.userName );
            
            // se the zone name from the conn
            ptr->zone_name( _comm->proxyUser.rodsZone );

            // set the socket from the conn
            //ptr->sock( _comm->sock );
        }
    }
    std::cout << "leaving openid_auth_client_start" << std::endl;
    return result;
}


// Auth request call on client side
// Sends auth request to server
irods::error openid_auth_client_request(
    irods::plugin_context& _ctx,
    rcComm_t*              _comm ) {
    std::cout << "entering openid_auth_client_request" << std::endl;
    irods::error ret;
    
    // validate incoming parameters
    if ( !_ctx.valid<irods::generic_auth_object>().ok() ) {
        return ERROR( SYS_INVALID_INPUT_PARAM, "Invalid plugin context." );
    }
    else if ( !_comm ) {
        return ERROR( SYS_INVALID_INPUT_PARAM, "null comm ptr" );
    }

    // get the auth object
    irods::generic_auth_object_ptr ptr = boost::dynamic_pointer_cast<irods::generic_auth_object>( _ctx.fco() );

    // get context string
    std::string context = ptr->context();
    
    // set up context string
    context += irods::kvp_delimiter() + irods::AUTH_USER_KEY + irods::kvp_association() + ptr->user_name(); 

    if ( context.size() > MAX_NAME_LEN ) {
        return ERROR( SYS_INVALID_INPUT_PARAM, "context string > max name len" );
    }

    // copy context to req in
    authPluginReqInp_t req_in;
    memset( &req_in, 0, sizeof(req_in) );
    strncpy( req_in.context_, context.c_str(), context.size() + 1 );
    
    // copy auth scheme to the req in struct TODO refactor
    std::string auth_scheme = "openid";
    strncpy( req_in.auth_scheme_, auth_scheme.c_str(), auth_scheme.size() + 1 );

    // TODO enable ssl on comm
    authPluginReqOut_t *req_out = 0;
    int status = rcAuthPluginRequest( _comm, &req_in, &req_out );
    
    // handle errors and exit
    if ( status < 0 ) {
        return ERROR( status, "call to rcAuthPluginRequest failed." );
    }
    else {
        // copy over resulting openid session token
        // and cache the result in our auth object
        ptr->request_result( req_out->result_ );
        obfSavePw( 0, 0, 0, req_out->result_ );
        free( req_out );
        std::cout << "leaving openid_auth_client_request" << std::endl;
        return SUCCESS();
    }
}

// Got request from client, send response back from server
irods::error openid_auth_client_response(
    irods::plugin_context& _ctx,
    rcComm_t*              _comm ) {
    std::cout << "entering openid_auth_client_response" << std::endl;
    irods::error result = SUCCESS();
    irods::error ret;

    // validate incoming parameters
    ret = _ctx.valid<irods::generic_auth_object>();
    if ( ( result = ASSERT_PASS( ret, "Invalid plugin context." ) ).ok() ) {
        if ( ( result = ASSERT_ERROR( _comm, SYS_INVALID_INPUT_PARAM, "Null comm pointer." ) ).ok() ) {
            // =-=-=-=-=-=-=-
            // get the auth object
            irods::generic_auth_object_ptr ptr = boost::dynamic_pointer_cast<irods::generic_auth_object>( _ctx.fco() );

            irods::kvp_map_t kvp;
            std::string auth_scheme_key = "openid";
            kvp[irods::AUTH_SCHEME_KEY] = auth_scheme_key; //TODO refactor
            std::string resp_str = irods::kvp_string( kvp );

            // =-=-=-=-=-=-=-
            // build the response string
            char response[ RESPONSE_LEN + 2 ];
            strncpy( response, resp_str.c_str(), RESPONSE_LEN + 2 );

            // =-=-=-=-=-=-=-
            // build the username#zonename string
            std::string user_name = ptr->user_name() + "#" + ptr->zone_name();
            char username[ MAX_NAME_LEN ];
            strncpy( username, user_name.c_str(), MAX_NAME_LEN );

            authResponseInp_t auth_response;
            auth_response.response = response;
            auth_response.username = username;
            int status = rcAuthResponse( _comm, &auth_response );
            result = ASSERT_ERROR( status >= 0, status, "Call to rcAuthResponse failed." );
        }
    }
    std::cout << "leaving openid_auth_client_response" << std::endl;
    return result; 
}

#ifdef RODS_SERVER
/*irods::error openid_establish_context_serverside(
    irods::plugin_context& _ctx,
    rsComm_t* comm,
    char* _clientName,
    int _maxLen_clientName) {
    irods::error result = SUCCESS();
    irods::error ret;

    ret = _ctx.valid<irods::generic_auth_object>();
    if ( ( result = ASSERT_PASS( ret, "Invalid plugin context." ) ).ok() ) {
        irods::generic_auth_object_ptr ptr = boost::dynamic_pointer_cast<irods::generic_auth_object>( _ctx.fco() );

    }

}*/

irods::error openid_auth_agent_start(
    irods::plugin_context& _ctx,
    const char*            _inst_name) {
    rodsLog( LOG_NOTICE, "entering openid_auth_agent_start");
    irods::error result = SUCCESS();
    irods::error ret;
    ret = _ctx.valid<irods::generic_auth_object>();
    
    if ( ( result = ASSERT_PASS( ret, "Invalid plugin context" ) ).ok() ) {
        irods::generic_auth_object_ptr ptr = boost::dynamic_pointer_cast<irods::generic_auth_object>( _ctx.fco() );
        int status;
        //char clientName[500];
        //genQueryInp_t genQueryInp;
        //genQueryOut_t *genQueryOut;
        //char condition1[MAX_NAME_LEN];
        //char condition2[MAX_NAME_LEN];
        //char *tResult;
        int privLevel = 0;
        int clientPrivLevel = 0;
        openidAuthReqStatus = 1;
        status = chkProxyUserPriv( _ctx.comm(), privLevel );
        if ( ( result = ASSERT_ERROR( status >= 0, status, "Failed checking proxy user priviledges." ) ).ok() ) {
            //openid_authorize();

            // set comm priv levels
            _ctx.comm()->proxyUser.authInfo.authFlag = privLevel;
            _ctx.comm()->clientUser.authInfo.authFlag = clientPrivLevel;

            // Reset the auth scheme here so we do not try to authenticate again unless the client requests it.
            if ( _ctx.comm()->auth_scheme != NULL ) {
                free( _ctx.comm()->auth_scheme );
            }
            _ctx.comm()->auth_scheme = NULL;        
        }
    }
    rodsLog( LOG_NOTICE, "leaving openid_auth_agent_start");
    return result;
}


irods::error openid_authorize() {
    //irods::generic_auth_object_ptr ptr = boost::dynamic_pointer_cast<irods::generic_auth_object>( _ctx.fco() );
        
    std::string provider_discovery_url = irods::get_server_property<std::string>("openid_provider_discovery_url");
    std::string client_id = irods::get_server_property<std::string>("openid_client_id");
    std::string client_secret = irods::get_server_property<std::string>("openid_client_secret");
    std::string redirect_uri = irods::get_server_property<std::string>("openid_redirect_uri");
    std::string authorization_endpoint;
    std::string token_endpoint;

    if ( !get_provider_metadata_field( provider_discovery_url, "authorization_endpoint", authorization_endpoint )
         || ! get_provider_metadata_field( provider_discovery_url, "token_endpoint", token_endpoint) ) {
        std::cout << "Provider discovery metadata missing fields" << std::endl;
        return ERROR(-1, "Provider discovery metadata missing fields");
    }

    std::string authorize_url_fmt = "%s?response_type=%s&scope=%s&client_id=%s&redirect_uri=%s";
    boost::format fmt(authorize_url_fmt);
    fmt % authorization_endpoint
        % "code"
        % "openid"
        % client_id
        % redirect_uri; 
        
    std::cout << "Waiting for OpenID provider authorization...\n" << authorize_url_fmt << std::endl;

    std::string *request_message = accept_request(8080);
    std::map<std::string,std::string> *param_map = get_params(*request_message);

    // check for code in callback
    if ( param_map->find("code") != param_map->end() ) {
        std::string authorization_code = param_map->at("code");
        std::string *access_token_response = get_access_token(
                                            token_endpoint,
                                            authorization_code,
                                            client_id,
                                            client_secret,
                                            redirect_uri);
        std::cout << *access_token_response << std::endl;
        std::stringstream response_stream(*access_token_response);
        boost::property_tree::ptree response_tree;
        boost::property_tree::read_json(response_stream, response_tree);
        if (response_tree.find("access_token") == response_tree.not_found()
            || response_tree.find("id_token") == response_tree.not_found()
            || response_tree.find("expires_in") == response_tree.not_found()) {
            std::cout << "Token response missing required fields (access_token, expires_in, id_token)" << std::endl;
        }
        else {
            std::string id_token = response_tree.get<std::string>("id_token");
            std::string access_token = response_tree.get<std::string>("access_token");
            std::string expires_in = response_tree.get<std::string>("expires_in");
            std::cout << "id_token: " << id_token << std::endl;
            // TODO base64 decode and validate fields 
            // https://openid.net/specs/openid-connect-core-1_0.html#IDTokenValidation
            std::cout << "access_token: " << access_token << std::endl;
            std::cout << "expires_in: " << expires_in << std::endl;
        }
        delete access_token_response;
    }
    else {
        return ERROR(-1, "Redirect callback missing required params");
    }
    std::cout << "leaving openid_authorize" << std::endl;
    return SUCCESS();
}


static irods::error _get_server_property(std::string key, std::string& buf) {
    try {
        buf = irods::get_server_property<std::string&>(key);
    }
    catch ( const irods::exception& e) {
        return irods::error(e);
    }
    return SUCCESS();
}

// server receives request from client
irods::error openid_auth_agent_request(
    irods::plugin_context& _ctx ) {
    std::cout << "entering openid_auth_agent_request" << std::endl;
    
    irods::error result = SUCCESS();
    irods::error ret;

    // validate incoming parameters
    ret = _ctx.valid<irods::generic_auth_object>();
    if ( ( result = ASSERT_PASS( ret, "Invalid plugin context." ) ).ok() ) {

        if ( openidAuthReqStatus == 1 ) {
            openidAuthReqStatus = 0;
            if ( !( result = ASSERT_ERROR( openidAuthReqError == 0, openidAuthReqError,
                                           "An openid auth request error has occurred." ) ).ok() ) {
                rodsLogAndErrorMsg( LOG_NOTICE, &_ctx.comm()->rError, openidAuthReqError,
                                    openidAuthReqErrorMsg );
            }
        }

        if ( result.ok() ) {
            irods::generic_auth_object_ptr ptr = boost::dynamic_pointer_cast<irods::generic_auth_object>( _ctx.fco() );
            std::string metadata_url;
            ret = _get_server_property("openid_provider_discovery_url", metadata_url);
            ret = SUCCESS();
            //ret = krb_kerberos_name(kerberos_name);
            if ( ( result = ASSERT_PASS( ret, "Failed to get openid provider discovery url from server config." ) ).ok() ) {
                std::string service_name;
                //ret = krb_setup_creds( ptr, false, kerberos_name, service_name );
                if ( ( result = ASSERT_PASS( ret, "Setting up openid credentials failed." ) ).ok() ) {
                    //_ctx.comm()->gsiRequest = 1;
                    if ( _ctx.comm()->auth_scheme != NULL ) {
                        free( _ctx.comm()->auth_scheme );
                    }
                    _ctx.comm()->auth_scheme = strdup( AUTH_OPENID_SCHEME.c_str() );
                    //ptr->service_name( service_name );
                    ptr->request_result( service_name );
                }
            }
        }
    }
    
    std::cout << "leaving openid_auth_agent_request" << std::endl;
    return SUCCESS();
}

static irods::error check_proxy_user_privileges(
    rsComm_t *rsComm,
    int proxyUserPriv ) {
    irods::error result = SUCCESS();

    if ( strcmp( rsComm->proxyUser.userName, rsComm->clientUser.userName ) != 0 ) {

        /* remote privileged user can only do things on behalf of users from
         * the same zone */
        result = ASSERT_ERROR( proxyUserPriv >= LOCAL_PRIV_USER_AUTH ||
                               ( proxyUserPriv >= REMOTE_PRIV_USER_AUTH &&
                                 strcmp( rsComm->proxyUser.rodsZone, rsComm->clientUser.rodsZone ) == 0 ),
                               SYS_PROXYUSER_NO_PRIV,
                               "Proxyuser: \"%s\" with %d no priv to auth clientUser: \"%s\".",
                               rsComm->proxyUser.userName, proxyUserPriv, rsComm->clientUser.userName );
    }
    return result;
}

irods::error openid_auth_agent_response(
    irods::plugin_context& _ctx,
    authResponseInp_t*     _resp ) {
    std::cout << "entering openid_auth_agent_response" << std::endl;
    irods::error result = SUCCESS();
    irods::error ret;

    // validate incoming parameters
    ret = _ctx.valid<irods::openid_auth_object>();
    if ( ( result = ASSERT_PASS( ret, "Invalid plugin context." ) ).ok() ) {
        int status;
        char *bufp;
        authCheckInp_t authCheckInp;
        authCheckOut_t *authCheckOut = NULL;
        rodsServerHost_t *rodsServerHost;

        char digest[RESPONSE_LEN + 2];
        char md5Buf[CHALLENGE_LEN + MAX_PASSWORD_LEN + 2];
        char serverId[MAX_PASSWORD_LEN + 2];
        MD5_CTX context;

        bufp = _rsAuthRequestGetChallenge();

        /* need to do NoLogin because it could get into inf loop for cross
         * zone auth */

        status = getAndConnRcatHostNoLogin( _ctx.comm(), MASTER_RCAT,
                                            _ctx.comm()->proxyUser.rodsZone, &rodsServerHost );
        if ( ( result = ASSERT_ERROR( status >= 0, status, "Connecting to rcat host failed." ) ).ok() ) {
            memset( &authCheckInp, 0, sizeof( authCheckInp ) );
            authCheckInp.challenge = bufp;
            authCheckInp.response = _resp->response;
            authCheckInp.username = _resp->username;

            if ( rodsServerHost->localFlag == LOCAL_HOST ) {
                status = rsAuthCheck( _ctx.comm(), &authCheckInp, &authCheckOut );
            }
            else {
                status = rcAuthCheck( rodsServerHost->conn, &authCheckInp, &authCheckOut );
                /* not likely we need this connection again */
                rcDisconnect( rodsServerHost->conn );
                rodsServerHost->conn = NULL;
            }
            if ( ( result = ASSERT_ERROR( status >= 0 && authCheckOut != NULL, status, "rcAuthCheck failed, status = %d.",
                                          status ) ).ok() ) { // JMC cppcheck

                if ( rodsServerHost->localFlag != LOCAL_HOST ) {
                    if ( authCheckOut->serverResponse == NULL ) {
                        rodsLog( LOG_NOTICE, "Warning, cannot authenticate remote server, no serverResponse field" );
                        result = ASSERT_ERROR( !requireServerAuth, REMOTE_SERVER_AUTH_NOT_PROVIDED, "Authentication disallowed. no serverResponse field." );
                    }

                    else {
                        char *cp;
                        int OK, len, i;
                        if ( *authCheckOut->serverResponse == '\0' ) {
                            rodsLog( LOG_NOTICE, "Warning, cannot authenticate remote server, serverResponse field is empty" );
                            result = ASSERT_ERROR( !requireServerAuth, REMOTE_SERVER_AUTH_EMPTY, "Authentication disallowed, empty serverResponse." );
                        }
                        else {
                            char username2[NAME_LEN + 2];
                            char userZone[NAME_LEN + 2];
                            memset( md5Buf, 0, sizeof( md5Buf ) );
                            strncpy( md5Buf, authCheckInp.challenge, CHALLENGE_LEN );
                            parseUserName( _resp->username, username2, userZone );
                            getZoneServerId( userZone, serverId );
                            len = strlen( serverId );
                            if ( len <= 0 ) {
                                rodsLog( LOG_NOTICE, "rsAuthResponse: Warning, cannot authenticate the remote server, no RemoteZoneSID defined in server.config", status );
                                result = ASSERT_ERROR( !requireServerAuth, REMOTE_SERVER_SID_NOT_DEFINED, "Authentication disallowed, no RemoteZoneSID defined." );
                            }
                            else {
                                strncpy( md5Buf + CHALLENGE_LEN, serverId, len );
                                MD5_Init( &context );
                                MD5_Update( &context, ( unsigned char* )md5Buf, CHALLENGE_LEN + MAX_PASSWORD_LEN );
                                MD5_Final( ( unsigned char* )digest, &context );
                                for ( i = 0; i < RESPONSE_LEN; i++ ) {
                                    if ( digest[i] == '\0' ) {
                                        digest[i]++;
                                    }  /* make sure 'string' doesn't
                                          end early*/
                                }
                                cp = authCheckOut->serverResponse;
                                OK = 1;
                                for ( i = 0; i < RESPONSE_LEN; i++ ) {
                                    if ( *cp++ != digest[i] ) {
                                        OK = 0;
                                    }
                                }
                                rodsLog( LOG_DEBUG, "serverResponse is OK/Not: %d", OK );
                                result = ASSERT_ERROR( OK != 0, REMOTE_SERVER_AUTHENTICATION_FAILURE, "Authentication disallowed, server response incorrect." );
                            }
                        }
                    }
                }

                /* Set the clientUser zone if it is null. */
                if ( result.ok() && strlen( _ctx.comm()->clientUser.rodsZone ) == 0 ) {
                    zoneInfo_t *tmpZoneInfo;
                    status = getLocalZoneInfo( &tmpZoneInfo );
                    if ( ( result = ASSERT_ERROR( status >= 0, status, "getLocalZoneInfo failed." ) ).ok() ) {
                        strncpy( _ctx.comm()->clientUser.rodsZone, tmpZoneInfo->zoneName, NAME_LEN );
                    }
                }

                /* have to modify privLevel if the icat is a foreign icat because
                 * a local user in a foreign zone is not a local user in this zone
                 * and vice versa for a remote user
                 */
                if ( result.ok() && rodsServerHost->rcatEnabled == REMOTE_ICAT ) {

                    /* proxy is easy because rodsServerHost is based on proxy user */
                    if ( authCheckOut->privLevel == LOCAL_PRIV_USER_AUTH ) {
                        authCheckOut->privLevel = REMOTE_PRIV_USER_AUTH;
                    }
                    else if ( authCheckOut->privLevel == LOCAL_USER_AUTH ) {
                        authCheckOut->privLevel = REMOTE_USER_AUTH;
                    }

                    /* adjust client user */
                    if ( strcmp( _ctx.comm()->proxyUser.userName,  _ctx.comm()->clientUser.userName ) == 0 ) {
                        authCheckOut->clientPrivLevel = authCheckOut->privLevel;
                    }
                    else {
                        zoneInfo_t *tmpZoneInfo;
                        status = getLocalZoneInfo( &tmpZoneInfo );
                        if ( ( result = ASSERT_ERROR( status >= 0, status, "getLocalZoneInfo failed." ) ).ok() ) {
                            if ( strcmp( tmpZoneInfo->zoneName,  _ctx.comm()->clientUser.rodsZone ) == 0 ) {
                                /* client is from local zone */
                                if ( authCheckOut->clientPrivLevel == REMOTE_PRIV_USER_AUTH ) {
                                    authCheckOut->clientPrivLevel = LOCAL_PRIV_USER_AUTH;
                                }
                                else if ( authCheckOut->clientPrivLevel == REMOTE_USER_AUTH ) {
                                    authCheckOut->clientPrivLevel = LOCAL_USER_AUTH;
                                }
                            }
                            else {
                                /* client is from remote zone */
                                if ( authCheckOut->clientPrivLevel == LOCAL_PRIV_USER_AUTH ) {
                                    authCheckOut->clientPrivLevel = REMOTE_USER_AUTH;
                                }
                                else if ( authCheckOut->clientPrivLevel == LOCAL_USER_AUTH ) {
                                    authCheckOut->clientPrivLevel = REMOTE_USER_AUTH;
                                }
                            }
                        }
                    }
                }
                else if ( strcmp( _ctx.comm()->proxyUser.userName,  _ctx.comm()->clientUser.userName ) == 0 ) {
                    authCheckOut->clientPrivLevel = authCheckOut->privLevel;
                }

                if ( result.ok() ) {
                    ret = check_proxy_user_privileges( _ctx.comm(), authCheckOut->privLevel );

                    if ( ( result = ASSERT_PASS( ret, "Check proxy user priviledges failed." ) ).ok() ) {
                        rodsLog( LOG_DEBUG,
                                 "rsAuthResponse set proxy authFlag to %d, client authFlag to %d, user:%s proxy:%s client:%s",
                                 authCheckOut->privLevel,
                                 authCheckOut->clientPrivLevel,
                                 authCheckInp.username,
                                 _ctx.comm()->proxyUser.userName,
                                 _ctx.comm()->clientUser.userName );

                        if ( strcmp( _ctx.comm()->proxyUser.userName,  _ctx.comm()->clientUser.userName ) != 0 ) {
                            _ctx.comm()->proxyUser.authInfo.authFlag = authCheckOut->privLevel;
                            _ctx.comm()->clientUser.authInfo.authFlag = authCheckOut->clientPrivLevel;
                        }
                        else {          /* proxyUser and clientUser are the same */
                            _ctx.comm()->proxyUser.authInfo.authFlag =
                                _ctx.comm()->clientUser.authInfo.authFlag = authCheckOut->privLevel;
                        }

                    }
                }
            }
        }
        if ( authCheckOut != NULL ) {
            if ( authCheckOut->serverResponse != NULL ) {
                free( authCheckOut->serverResponse );
            }

            free( authCheckOut );
        }
    }
    std::cout << "leaving openid_auth_agent_response" << std::endl;
    return SUCCESS();
}

irods::error openid_auth_agent_verify(
    irods::plugin_context& _ctx,
    const char*            _challenge,
    const char*            _user_name,
    const char*            _response ) {
    std::cout << "entering openid_auth_agent_verify" << std::endl;

    std::cout << "leaving openid_auth_agent_verify" << std::endl;
    return SUCCESS();
}
#endif

/// @brief The openid auth plugin
class openid_auth_plugin : public irods::auth {
public:
    /// @brief Constructor
    openid_auth_plugin(
        const std::string& _name, // instance name
        const std::string& _ctx   // context
        ) : irods::auth( _name, _ctx ) { }

    /// @brief Destructor
    ~openid_auth_plugin() { }

}; // class openid_auth_plugin

/// @brief factory function to provide an instance of the plugin
extern "C"
irods::auth* plugin_factory(
    const std::string& _inst_name,
    const std::string& _context ) {
    using namespace irods;

    openid_auth_plugin* openid = new openid_auth_plugin( _inst_name, _context );
    if(!openid) {
        rodsLog(
            LOG_ERROR,
            "failed to create openid auth plugin");
        return nullptr;
    }

    openid->add_operation(
        irods::AUTH_ESTABLISH_CONTEXT,
        std::function<error(plugin_context&)>(
            openid_auth_establish_context) );
    openid->add_operation<rcComm_t*,const char*>(
        irods::AUTH_CLIENT_START,
        std::function<error(
            plugin_context&,
            rcComm_t*,
            const char*)>(openid_auth_client_start));
    openid->add_operation<rcComm_t*>(
        irods::AUTH_CLIENT_AUTH_REQUEST,
        std::function<error(plugin_context&,rcComm_t*)>(
            openid_auth_client_request ));
    openid->add_operation<rcComm_t*>(
        irods::AUTH_CLIENT_AUTH_RESPONSE,
        std::function<error(plugin_context&,rcComm_t*)>(
            openid_auth_client_response ));

#ifdef RODS_SERVER
    openid->add_operation<const char*>(
        irods::AUTH_AGENT_START,
        std::function<error(plugin_context&,const char*)>(
            openid_auth_agent_start) );
    openid->add_operation(
        irods::AUTH_AGENT_AUTH_REQUEST,
        std::function<error(plugin_context&)>(
            openid_auth_agent_request ));
    openid->add_operation<authResponseInp_t*>(
       irods::AUTH_AGENT_AUTH_RESPONSE,
       std::function<error(plugin_context&,authResponseInp_t*)>(
           openid_auth_agent_response) );
    openid->add_operation<const char*,const char*,const char*>(
       irods::AUTH_AGENT_AUTH_VERIFY,
       std::function<error(
           plugin_context&,
           const char*,
           const char*,
           const char*)>(
               openid_auth_agent_verify) );
#endif

    return openid;

} // plugin_factory


// OPENID helper methods
std::string *curl_post(std::string, std::string *);
std::string *curl_get(std::string, std::string *);

void send_success(int sockfd)
{
    std::string msg =
        "HTTP/1.1 200 OK\n"
        "Content-Type: text/html; encoding=utf8\n"
        "Content-Length: 53\n"
        "Connection: close\n\n"
        "<html><head></head><body><p>Success</p></body></html>";
    send(sockfd, msg.c_str(), msg.length(), 0);
}

/* Given a fully qualified url to a discovery document for an OpenID Identity Provider,
 * send a GET request for that document and put it into a boost ptree.
 */
boost::property_tree::ptree *get_provider_metadata(std::string provider_metadata_url)
{
    boost::property_tree::ptree *metadata_tree = new boost::property_tree::ptree();
    std::string params = "";
    std::string *metadata_string = curl_get(provider_metadata_url, &params);
    std::cout << "Provider metadata: " << std::endl << *metadata_string << std::endl;
    std::stringstream metadata_stream(*metadata_string);
    
    boost::property_tree::read_json(metadata_stream, *metadata_tree);
   
    const char *required_fields[] = {
        "issuer",
        "authorization_endpoint",
        "token_endpoint",
        "userinfo_endpoint",
        "scopes_supported",
        "response_types_supported",
        "claims_supported"};
    std::vector<std::string> metadata_required(required_fields, std::end(required_fields));
    for (std::vector<std::string>::iterator field_iter = metadata_required.begin(); field_iter != metadata_required.end(); ++field_iter)
    {
        if (metadata_tree->find(*field_iter) == metadata_tree->not_found())
        {
            std::cout << "Metadata tree missing required field: " << *field_iter << std::endl;
            delete metadata_tree;
            metadata_tree = NULL;
            break;
        }
    }
 
    delete metadata_string;
    return metadata_tree;
}

// TODO
// static std::vector<boost::property_tree::ptree> provider_discovery_metadata_cache;

bool get_provider_metadata_field(std::string provider_metadata_url, const std::string fieldname, std::string& value)
{
    boost::property_tree::ptree *metadata_tree = get_provider_metadata(provider_metadata_url);
    if (metadata_tree->find(fieldname) != metadata_tree->not_found())
    {
        value = metadata_tree->get<std::string>(fieldname);
        return true;
    }
    else
    {
        return false;
    }
}


/* Takes a GET request string. This is the literal string representation of the request.
 * Looks for the line with the request path, and splits it up into pair<key, value> for each request parameter
 * If the key has no value, the value part of the pair is left as an empty string. 
 * Returns a map<string,string> of each request parameter
 */
std::map<std::string,std::string> *get_params(std::string req)
{
    std::map<std::string,std::string> *req_map = new std::map<std::string,std::string>();
    std::vector<std::string> split_vector;
    boost::split(split_vector, req, boost::is_any_of("\r\n"), boost::token_compress_on);
    // iterate over lines in the request string
    for (std::vector<std::string>::iterator line_iter = split_vector.begin(); line_iter != split_vector.end(); ++line_iter)
    {
        std::string line = *line_iter;
        //cout << "Request line: " << line << endl;
        if (std::regex_match(line, std::regex("GET /.*"))) { // can require path here
            std::vector<std::string> method_path_params_version_vector;
            boost::split(method_path_params_version_vector, line, boost::is_any_of(" "), boost::token_compress_on);
            if (method_path_params_version_vector.size() >= 2)
            {
                std::string path_params = method_path_params_version_vector.at(1);
                size_t param_start = path_params.find_first_of("?", 0);
                if (param_start == std::string::npos)
                {
                    std::cout << "Request had no parameters" << std::endl;
                    break;
                }
                std::string params = path_params.substr(param_start+1, std::string::npos);
                
                std::vector<std::string> param_vector;
                boost::split(param_vector, params, boost::is_any_of("&"), boost::token_compress_on);
                // iterate over parameters in the request path
                for (std::vector<std::string>::iterator param_iter = param_vector.begin(); param_iter != param_vector.end(); ++param_iter) {
                    std::string param = *param_iter;
                    std::vector<std::string> key_value_vector;
                    // split the parameter into [name, value], or [name] if no value exists
                    boost::split(key_value_vector, param, boost::is_any_of("="), boost::token_compress_on);
                    if (key_value_vector.size() == 2)
                    {
                        req_map->insert(std::pair<std::string,std::string>(key_value_vector.at(0), key_value_vector.at(1)));
                    }
                    else if (key_value_vector.size() == 1)
                    {
                        req_map->insert(std::pair<std::string,std::string>(key_value_vector.at(0), ""));
                    }
                }
            }
            else
            {
                std::cout << "GET line had " << method_path_params_version_vector.size() << " terms" << std::endl;
                // error
            }
        }
    }

    return req_map;
}

static size_t _curl_writefunction_callback(void *contents, size_t size, size_t nmemb, void *s)
{
    ((std::string*)s)->append((char*)contents, size * nmemb);
    return size * nmemb;
}


/* Return a string response from a post call
 * token_endpoint_url = "https://www.googleapis.com/oauth2/v4/token"
 * client_id = "118582272506-6vm4rruieahekajob0tghgdf3iogtdgt.apps.googleusercontent.com"
 * client_secret = "el7Fkt1q4KMozJ-M9uNJebWz"
 * redirect_uri = "http://localhost:8080"
 * authorization_code = <different per authorization request>
 */
std::string *get_access_token(std::string token_endpoint_url, 
                              std::string authorization_code, 
                              std::string client_id, 
                              std::string client_secret, 
                              std::string redirect_uri)
{
    std::stringstream fields;
    fields << "code=" << authorization_code;
    fields << "&client_id=" << client_id;
    fields << "&client_secret=" << client_secret;
    fields << "&redirect_uri=" << redirect_uri;
    fields << "&grant_type=" << "authorization_code";
    std::string *field_str = new std::string(fields.str());
    std::string *response = curl_post(token_endpoint_url, field_str);
    delete field_str;
    return response;
}


std::string *curl_post(std::string url, std::string *fields)
{
    CURL *curl;
    CURLcode res;
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    std::string *response = new std::string();
    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_writefunction_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, fields->length());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields->c_str());

        std::cout << "Performing curl" << std::endl;
        res = curl_easy_perform(curl);
        if (res != CURLE_OK)
        {
            fprintf(stderr, "curl_easy_perform() failed %s\n", curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    return response;
}

std::string *curl_get(std::string url, std::string *params)
{
    CURL *curl;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    std::string *response = new std::string();
    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_writefunction_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
        res = curl_easy_perform(curl);
        if (res != CURLE_OK)
        {
            fprintf(stderr, "curl_easy_perform() failed %s\n", curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    return response;
}

std::string *accept_request(int portno)
{
    int sockfd;
    struct sockaddr_in server_address;
    struct sockaddr_in client_address;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(8080);
    bind(sockfd, (struct sockaddr *)&server_address, sizeof(server_address));
    listen(sockfd, 1);
    std::string *message = new std::string("");
    
    // set up connection socket
    socklen_t socksize = sizeof(client_address);
    int conn_sock_fd = accept(sockfd, (struct sockaddr *)&client_address, &socksize);
    const size_t BUF_LEN = 2048;
    char buf[BUF_LEN+1]; buf[BUF_LEN] = 0x0;
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(conn_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    while (1)
    {
        int received_len = recv(conn_sock_fd, buf, BUF_LEN, 0);
        std::cout << "Received " << received_len << std::endl;
        if (received_len == -1)
        {
            // EAGAIN EWOULDBLOCK
            std::cout << "Timeout reached" << std::endl;
            send_success(conn_sock_fd);
            break;
        }
        if (received_len == 0)
        {
            std::cout << "Closing connection" << std::endl;
            send_success(conn_sock_fd);
            close(conn_sock_fd);
            break;
        }
        message->append(buf); 
    }
    close(sockfd);
    return message;
}

int main(int argc, char **argv)
{
    boost::property_tree::ptree *metadata_tree = get_provider_metadata("https://accounts.google.com/.well-known/openid-configuration");
    if ( !metadata_tree )
    {
        return 1;
    }
    std::string provider_authorization_endpoint = metadata_tree->get<std::string>("authorization_endpoint");
    std::string provider_token_endpoint = metadata_tree->get<std::string>("token_endpoint");
    std::string provider_userinfo_endpoint = metadata_tree->get<std::string>("userinfo_endpoint");

    std::string client_id = "118582272506-6vm4rruieahekajob0tghgdf3iogtdgt.apps.googleusercontent.com";
    std::string client_secret = "el7Fkt1q4KMozJ-M9uNJebWz";

    std::string authorize_url_fmt = "%s?response_type=%s&scope=%s&client_id=%s&redirect_uri=%s";
    boost::format fmt(authorize_url_fmt);
    fmt % provider_authorization_endpoint
        % "code"
        % "openid"
        % client_id
        % "http://localhost:8080";
    std::string authorize_url = fmt.str();

    std::cout << "Waiting for OpenID provider authorization...\n" << authorize_url << std::endl;

    while (1)
    {
        std::string *message = accept_request(8080);
        std::cout << *message << std::endl;
        std::map<std::string,std::string> *param_map = get_params(*message);
        if (param_map->find("code") != param_map->end())
        {
            std::string authorization_code = param_map->at("code");
            //cout << "Using authorization code to retrieve OAuth2 access token" << endl;
            std::string *access_token_response = get_access_token(
                                                provider_token_endpoint,
                                                authorization_code,
                                                client_id,
                                                client_secret,
                                                "http://localhost:8080");

            std::cout << *access_token_response;
            std::stringstream response_stream(*access_token_response);
            
            boost::property_tree::ptree response_tree;
            boost::property_tree::read_json(response_stream, response_tree);
            if (response_tree.find("access_token") == response_tree.not_found() 
                  || response_tree.find("id_token") == response_tree.not_found()
                  || response_tree.find("expires_in") == response_tree.not_found()) 
            {
                std::cout << "Token response missing required fields (access_token, expires_in, id_token)" << std::endl;
            }
            else
            {
                std::string id_token = response_tree.get<std::string>("id_token");
                std::string access_token = response_tree.get<std::string>("access_token");
                std::string expires_in = response_tree.get<std::string>("expires_in");
                std::cout << "id_token: " << id_token << std::endl;
                // TODO base64 decode and validate fields 
                // https://openid.net/specs/openid-connect-core-1_0.html#IDTokenValidation
                std::cout << "access_token: " << access_token << std::endl;
                std::cout << "expires_in: " << expires_in << std::endl;
            }
            delete access_token_response;
        }
        else
        {
            std::cout << "Request did not contain an authorization code" << std::endl;
        }
        
        delete param_map;
    } // end while
} // end main


