/*
    Yojimbo Client/Server Network Library.
    
    Copyright © 2016, The Network Protocol Company, Inc.

    All rights reserved.
*/

#include "yojimbo_client_server.h"
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

namespace yojimbo
{
    void GenerateConnectToken( ConnectToken & token, uint64_t clientId, int numServerAddresses, const Address * serverAddresses, uint32_t protocolId )
    {
        uint64_t timestamp = (uint64_t) time( NULL );
        
        token.protocolId = protocolId;
        token.clientId = clientId;
        token.expiryTimestamp = timestamp + ConnectTokenExpirySeconds;
        
        assert( numServerAddresses > 0 );
        assert( numServerAddresses <= MaxServersPerConnectToken );
        token.numServerAddresses = numServerAddresses;
        for ( int i = 0; i < numServerAddresses; ++i )
            token.serverAddresses[i] = serverAddresses[i];

        GenerateKey( token.clientToServerKey );    

        GenerateKey( token.serverToClientKey );

        GenerateKey( token.random );
    }

    bool EncryptConnectToken( ConnectToken & token, uint8_t *encryptedMessage, const uint8_t *additional, int additionalLength, const uint8_t * nonce, const uint8_t * key )
    {
        uint8_t message[ConnectTokenBytes];
        memset( message, 0, ConnectTokenBytes );
        WriteStream stream( message, ConnectTokenBytes );
        if ( !token.Serialize( stream ) )
            return false;

        stream.Flush();
        
        if ( stream.GetError() )
            return false;

        uint64_t encryptedLength;

        if ( !Encrypt_AEAD( message, ConnectTokenBytes - AuthBytes, encryptedMessage, encryptedLength, additional, additionalLength, nonce, key ) )
            return false;

        assert( encryptedLength == ConnectTokenBytes );

        return true;
    }

    bool DecryptConnectToken( const uint8_t * encryptedMessage, ConnectToken & decryptedToken, const uint8_t * additional, int additionalLength, const uint8_t * nonce, const uint8_t * key )
    {
        const int encryptedMessageLength = ConnectTokenBytes;

        uint64_t decryptedMessageLength;
        uint8_t decryptedMessage[ConnectTokenBytes];

        if ( !Decrypt_AEAD( encryptedMessage, encryptedMessageLength, decryptedMessage, decryptedMessageLength, additional, additionalLength, nonce, key ) )
            return false;

        assert( decryptedMessageLength == ConnectTokenBytes - AuthBytes );

        ReadStream stream( decryptedMessage, ConnectTokenBytes - AuthBytes );
        if ( !decryptedToken.Serialize( stream ) )
            return false;

        if ( stream.GetError() )
            return false;

        return true;
    }

    bool GenerateChallengeToken( const ConnectToken & connectToken, const Address & clientAddress, const Address & serverAddress, const uint8_t * connectTokenMac, ChallengeToken & challengeToken )
    {
        if ( connectToken.clientId == 0 )
            return false;

        if ( !clientAddress.IsValid() )
            return false;

        challengeToken.clientId = connectToken.clientId;

        challengeToken.clientAddress = clientAddress;
        
        challengeToken.serverAddress = serverAddress;

        memcpy( challengeToken.connectTokenMac, connectTokenMac, MacBytes );

        memcpy( challengeToken.clientToServerKey, connectToken.clientToServerKey, KeyBytes );

        memcpy( challengeToken.serverToClientKey, connectToken.serverToClientKey, KeyBytes );

        GenerateKey( challengeToken.random );

        return true;
    }

    bool EncryptChallengeToken( ChallengeToken & token, uint8_t *encryptedMessage, const uint8_t *additional, int additionalLength, const uint8_t * nonce, const uint8_t * key )
    {
        uint8_t message[ChallengeTokenBytes];
        memset( message, 0, ChallengeTokenBytes );
        WriteStream stream( message, ChallengeTokenBytes );
        if ( !token.Serialize( stream ) )
            return false;

        stream.Flush();
        
        if ( stream.GetError() )
            return false;

        uint64_t encryptedLength;

        if ( !Encrypt_AEAD( message, ChallengeTokenBytes - AuthBytes, encryptedMessage, encryptedLength, additional, additionalLength, nonce, key ) )
            return false;

        assert( encryptedLength == ChallengeTokenBytes );

        return true;
    }

    bool DecryptChallengeToken( const uint8_t * encryptedMessage, ChallengeToken & decryptedToken, const uint8_t * additional, int additionalLength, const uint8_t * nonce, const uint8_t * key )
    {
        const int encryptedMessageLength = ChallengeTokenBytes;

        uint64_t decryptedMessageLength;
        uint8_t decryptedMessage[ChallengeTokenBytes];

        if ( !Decrypt_AEAD( encryptedMessage, encryptedMessageLength, decryptedMessage, decryptedMessageLength, additional, additionalLength, nonce, key ) )
            return false;

        assert( decryptedMessageLength == ChallengeTokenBytes - AuthBytes );

        ReadStream stream( decryptedMessage, ChallengeTokenBytes - AuthBytes );
        if ( !decryptedToken.Serialize( stream ) )
            return false;

        if ( stream.GetError() )
            return false;

        return true;
    }

    // =============================================================

    Server::Server( NetworkInterface & networkInterface )
    {
        memset( m_privateKey, 0, KeyBytes );
        m_networkInterface = &networkInterface;
        m_numConnectedClients = 0;
        m_challengeTokenNonce = 0;
        for ( int i = 0; i < MaxClients; ++i )
            ResetClientState( i );
    }

    Server::~Server()
    {
        assert( m_networkInterface );
        m_networkInterface = NULL;
    }

    void Server::SendPackets( double time )
    {
        for ( int i = 0; i < MaxClients; ++i )
        {
            if ( !m_clientConnected[i] )
                continue;

            if ( m_clientData[i].lastPacketSendTime + ConnectionHeartBeatRate > time )
                return;

            ConnectionHeartBeatPacket * packet = (ConnectionHeartBeatPacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_HEARTBEAT );

            SendPacketToConnectedClient( i, packet, time );
        }
    }

    void Server::ReceivePackets( double time )
    {
        while ( true )
        {
            Address address;
            Packet *packet = m_networkInterface->ReceivePacket( address );
            if ( !packet )
                break;
            
            switch ( packet->GetType() )
            {
                case PACKET_CONNECTION_REQUEST:
                    ProcessConnectionRequest( *(ConnectionRequestPacket*)packet, address, time );
                    break;

                case PACKET_CONNECTION_RESPONSE:
                    ProcessConnectionResponse( *(ConnectionResponsePacket*)packet, address, time );
                    break;

                case PACKET_CONNECTION_HEARTBEAT:
                    ProcessConnectionHeartBeat( *(ConnectionHeartBeatPacket*)packet, address, time );
                    break;

                case PACKET_CONNECTION_DISCONNECT:
                    ProcessConnectionDisconnect( *(ConnectionDisconnectPacket*)packet, address, time );
                    break;

                default:
                    break;
            }

            m_networkInterface->DestroyPacket( packet );
        }
    }

    void Server::CheckForTimeOut( double time )
    {
        for ( int i = 0; i < MaxClients; ++i )
        {
            if ( !m_clientConnected[i] )
                continue;

            if ( m_clientData[i].lastPacketReceiveTime + ConnectionTimeOut < time )
            {
                OnClientTimedOut( i );
                DisconnectClient( i, time );
            }
        }
    }

    void Server::ResetClientState( int clientIndex )
    {
        assert( clientIndex >= 0 );
        assert( clientIndex < MaxClients );
        m_clientConnected[clientIndex] = false;
        m_clientId[clientIndex] = 0;
        m_clientAddress[clientIndex] = Address();
        m_clientData[clientIndex] = ServerClientData();
    }

    int Server::FindFreeClientIndex() const
    {
        for ( int i = 0; i < MaxClients; ++i )
        {
            if ( !m_clientConnected[i] )
                return i;
        }
        return -1;
    }

    int Server::FindExistingClientIndex( const Address & address ) const
    {
        for ( int i = 0; i < MaxClients; ++i )
        {
            if ( m_clientConnected[i] && m_clientAddress[i] == address )
                return i;
        }
        return -1;
    }

    int Server::FindExistingClientIndex( const Address & address, uint64_t clientId ) const
    {
        for ( int i = 0; i < MaxClients; ++i )
        {
            if ( m_clientId[i] == clientId && m_clientConnected[i] && m_clientAddress[i] == address )
                return i;
        }
        return -1;
    }

    bool Server::FindOrAddConnectTokenEntry( const Address & address, const uint8_t * mac, double time )
    {
        // find the matching entry for the token mac, and the oldest token. constant time worst case O(1) at all times. This is intentional!

        assert( address.IsValid() );

        assert( mac );

        int matchingTokenIndex = -1;
        int oldestTokenIndex = -1;
        double oldestTokenTime = 0.0;
        for ( int i = 0; i < MaxConnectTokenEntries; ++i )
        {
            if ( memcmp( mac, m_connectTokenEntries[i].mac, MacBytes ) == 0 )
            {
                matchingTokenIndex = i;
            }

            if ( oldestTokenIndex == -1 || oldestTokenTime < m_connectTokenEntries[i].time )
            {
                oldestTokenTime = m_connectTokenEntries[i].time;
                oldestTokenIndex = i;
            }
        }

        // if no entry is found with the mac, replace the oldest entry with this (mac,address,time) and return true

        assert( oldestTokenIndex != -1 );

        if ( matchingTokenIndex == -1 )
        {
            m_connectTokenEntries[oldestTokenIndex].time = time;
            m_connectTokenEntries[oldestTokenIndex].address = address;
            memcpy( m_connectTokenEntries[oldestTokenIndex].mac, mac, MacBytes );
            return true;
        }

        // if an entry is found with the same mac *and* it has the same address, return true

        assert( matchingTokenIndex >= 0 );
        assert( matchingTokenIndex < MaxConnectTokenEntries );

        if ( m_connectTokenEntries[matchingTokenIndex].address == address )
            return true;

        // otherwise an entry exists with the same mac but a different address, somebody is trying to reuse the connect token as a replay attack!

        return false;
    }

    void Server::ConnectClient( int clientIndex, const ChallengeToken & challengeToken, double time )
    {
        assert( m_numConnectedClients >= 0 );
        assert( m_numConnectedClients < MaxClients - 1 );
        assert( !m_clientConnected[clientIndex] );

        m_numConnectedClients++;

        m_clientConnected[clientIndex] = true;
        m_clientId[clientIndex] = challengeToken.clientId;
        m_clientAddress[clientIndex] = challengeToken.clientAddress;

        m_clientData[clientIndex].address = challengeToken.clientAddress;
        m_clientData[clientIndex].clientId = challengeToken.clientId;
        m_clientData[clientIndex].connectTime = time;
        m_clientData[clientIndex].lastPacketSendTime = time;
        m_clientData[clientIndex].lastPacketReceiveTime = time;

        OnClientConnect( clientIndex );

        ConnectionHeartBeatPacket * connectionHeartBeatPacket = (ConnectionHeartBeatPacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_HEARTBEAT );

        if ( connectionHeartBeatPacket )
        {
            SendPacketToConnectedClient( clientIndex, connectionHeartBeatPacket, time );
        }
    }

    void Server::DisconnectClient( int clientIndex, double time )
    {
        assert( clientIndex >= 0 );
        assert( clientIndex < MaxClients );
        assert( m_numConnectedClients > 0 );
        assert( m_clientConnected[clientIndex] );

        OnClientDisconnect( clientIndex );

        ConnectionDisconnectPacket * packet = (ConnectionDisconnectPacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_DISCONNECT );

        SendPacketToConnectedClient( clientIndex, packet, time );

        ResetClientState( clientIndex );

        m_numConnectedClients--;
    }

    bool Server::IsConnected( uint64_t clientId ) const
    {
        for ( int i = 0; i < MaxClients; ++i )
        {
            if ( !m_clientConnected[i] )
                continue;
            if ( m_clientId[i] == clientId )
                return true;
        }
        return false;
    }

    bool Server::IsConnected( const Address & address, uint64_t clientId ) const
    {
        for ( int i = 0; i < MaxClients; ++i )
        {
            if ( !m_clientConnected[i] )
                continue;
            if ( m_clientAddress[i] == address && m_clientId[i] == clientId )
                return true;
        }
        return false;
    }

    void Server::SendPacketToConnectedClient( int clientIndex, Packet * packet, double time )
    {
        assert( packet );
        assert( clientIndex >= 0 );
        assert( clientIndex < MaxClients );
        assert( m_clientConnected[clientIndex] );
        m_clientData[clientIndex].lastPacketSendTime = time;
        m_networkInterface->SendPacket( m_clientAddress[clientIndex], packet );
    }

    void Server::ProcessConnectionRequest( const ConnectionRequestPacket & packet, const Address & address, double time )
    {
        /*
        char buffer[256];
        const char * addressString = address.ToString( buffer, sizeof( buffer ) );        
        printf( "processing connection request packet from: %s\n", addressString );
        */

        ConnectToken connectToken;
        if ( !DecryptConnectToken( packet.connectTokenData, connectToken, NULL, 0, packet.connectTokenNonce, m_privateKey ) )
        {
            printf( "failed to decrypt connect token\n" );
            return;
        }

        bool serverAddressInConnectTokenWhiteList = false;

        for ( int i = 0; i < connectToken.numServerAddresses; ++i )
        {
            if ( m_serverAddress == connectToken.serverAddresses[i] )
            {
                serverAddressInConnectTokenWhiteList = true;
                break;
            }
        }

        if ( !serverAddressInConnectTokenWhiteList )
        {
            printf( "server address is not in connect token whitelist\n" );
            return;
        }

        if ( connectToken.clientId == 0 )
        {
            printf( "connect token client id is 0\n" );
            return;
        }

        if ( IsConnected( address, connectToken.clientId ) )
        {
            printf( "client is already connected\n" );
            return;
        }

        uint64_t timestamp = (uint64_t) ::time( NULL );

        if ( connectToken.expiryTimestamp <= timestamp )
        {
            printf( "connect token has expired\n" );
            return;
        }

        if ( !m_networkInterface->AddEncryptionMapping( address, connectToken.serverToClientKey, connectToken.clientToServerKey ) )
        {
            printf( "failed to add encryption mapping\n" );
            return;
        }

        if ( m_numConnectedClients == MaxClients )
        {
            printf( "connection denied: server is full\n" );
            ConnectionDeniedPacket * connectionDeniedPacket = (ConnectionDeniedPacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_DENIED );
            m_networkInterface->SendPacket( address, connectionDeniedPacket );
            return;
        }

        if ( !FindOrAddConnectTokenEntry( address, packet.connectTokenData, time ) )
        {
            printf( "connect token has already been used\n" );
            return;
        }

        ChallengeToken challengeToken;
        if ( !GenerateChallengeToken( connectToken, address, m_serverAddress, packet.connectTokenData, challengeToken ) )
        {
            printf( "failed to generate challenge token\n" );
            return;
        }

        ConnectionChallengePacket * connectionChallengePacket = (ConnectionChallengePacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_CHALLENGE );
        if ( !connectionChallengePacket )
            return;

        memcpy( connectionChallengePacket->challengeTokenNonce, (uint8_t*) &m_challengeTokenNonce, NonceBytes );

        if ( !EncryptChallengeToken( challengeToken, connectionChallengePacket->challengeTokenData, NULL, 0, connectionChallengePacket->challengeTokenNonce, m_privateKey ) )
        {
            printf( "failed to encrypt challenge token\n" );
            return;
        }

        char clientAddressString[64];
        address.ToString( clientAddressString, sizeof( clientAddressString ) );
        printf( "server sent challenge to client %s\n", clientAddressString );

        m_networkInterface->SendPacket( address, connectionChallengePacket );
    }

    void Server::ProcessConnectionResponse( const ConnectionResponsePacket & packet, const Address & address, double time )
    {
        ChallengeToken challengeToken;
        if ( !DecryptChallengeToken( packet.challengeTokenData, challengeToken, NULL, 0, packet.challengeTokenNonce, m_privateKey ) )
        {
            printf( "failed to decrypt challenge token\n" );
            return;
        }

        if ( challengeToken.clientAddress != address )
        {
            printf( "challenge token client address does not match\n" );
            return;
        }

        if ( challengeToken.serverAddress != m_serverAddress )
        {
            printf( "challenge token server address does not match\n" );
            return;
        }

        const int existingClientIndex = FindExistingClientIndex( address, challengeToken.clientId );
        if ( existingClientIndex != -1 )
        {
            assert( existingClientIndex >= 0 );
            assert( existingClientIndex < MaxClients );

            if ( m_clientData[existingClientIndex].lastPacketSendTime + ConnectionConfirmSendRate < time )
            {
                ConnectionHeartBeatPacket * connectionHeartBeatPacket = (ConnectionHeartBeatPacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_HEARTBEAT );

                if ( connectionHeartBeatPacket )
                {
                    SendPacketToConnectedClient( existingClientIndex, connectionHeartBeatPacket, time );
                }
            }

            return;
        }

        char buffer[256];
        const char * addressString = address.ToString( buffer, sizeof( buffer ) );
        printf( "processing connection response from client %s (client id = %" PRIx64 ")\n", addressString, challengeToken.clientId );

        if ( m_numConnectedClients == MaxClients )
        {
            printf( "connection denied: server is full\n" );
            ConnectionDeniedPacket * connectionDeniedPacket = (ConnectionDeniedPacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_DENIED );
            if ( connectionDeniedPacket )
            {
                m_networkInterface->SendPacket( address, connectionDeniedPacket );
            }
            return;
        }

        const int clientIndex = FindFreeClientIndex();

        assert( clientIndex != -1 );
        if ( clientIndex == -1 )
            return;

        ConnectClient( clientIndex, challengeToken, time );
    }

    void Server::ProcessConnectionHeartBeat( const ConnectionHeartBeatPacket & /*packet*/, const Address & address, double time )
    {
        const int clientIndex = FindExistingClientIndex( address );
        if ( clientIndex == -1 )
            return;

        assert( clientIndex >= 0 );
        assert( clientIndex < MaxClients );

        m_clientData[clientIndex].lastPacketReceiveTime = time;
    }

    void Server::ProcessConnectionDisconnect( const ConnectionDisconnectPacket & /*packet*/, const Address & address, double time )
    {
        const int clientIndex = FindExistingClientIndex( address );
        if ( clientIndex == -1 )
            return;

        assert( clientIndex >= 0 );
        assert( clientIndex < MaxClients );

        DisconnectClient( clientIndex, time );
    }

    // =============================================================

    Client::Client( NetworkInterface & networkInterface )
    {
        m_networkInterface = &networkInterface;
        ResetConnectionData();
    }

    Client::~Client()
    {
        m_networkInterface = NULL;
    }

    void Client::Connect( const Address & address, 
                          double time, 
                          uint64_t clientId,
                          const uint8_t * connectTokenData, 
                          const uint8_t * connectTokenNonce,
                          const uint8_t * clientToServerKey,
                          const uint8_t * serverToClientKey )
    {
        Disconnect( time );
        m_serverAddress = address;
        m_clientState = CLIENT_STATE_SENDING_CONNECTION_REQUEST;
        m_lastPacketSendTime = time - 1.0f;
        m_lastPacketReceiveTime = time;
        m_clientId = clientId;
        memcpy( m_connectTokenData, connectTokenData, ConnectTokenBytes );
        memcpy( m_connectTokenNonce, connectTokenNonce, NonceBytes );
        m_networkInterface->ResetEncryptionMappings();
        m_networkInterface->AddEncryptionMapping( m_serverAddress, clientToServerKey, serverToClientKey );
    }

    void Client::Disconnect( double time )
    {
        if ( m_clientState == CLIENT_STATE_CONNECTED )
        {
            printf( "client-side disconnect: (client id = %" PRIx64 ")\n", m_clientId );

            ConnectionDisconnectPacket * packet = (ConnectionDisconnectPacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_DISCONNECT );
            
            if ( packet )
            {
                SendPacketToServer( packet, time );

                // todo: for the server-side version of this we probably want "WriteFlush" that takes an address for all packets to flush.

                // IMPORTANT: flush the disconnect packet to the network *before* the encryption mapping is reset!
                m_networkInterface->WritePackets( time );      
            }
        }

        ResetConnectionData();
    }

    void Client::SendPackets( double time )
    {
        switch ( m_clientState )
        {
            case CLIENT_STATE_SENDING_CONNECTION_REQUEST:
            {
                if ( m_lastPacketSendTime + ConnectionRequestSendRate > time )
                    return;

                char buffer[256];
                const char *addressString = m_serverAddress.ToString( buffer, sizeof( buffer ) );
                printf( "client sending connection request to server %s\n", addressString );

                ConnectionRequestPacket * packet = (ConnectionRequestPacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_REQUEST );

                if ( packet )
                {
                    memcpy( packet->connectTokenData, m_connectTokenData, ConnectTokenBytes );
                    memcpy( packet->connectTokenNonce, m_connectTokenNonce, NonceBytes );

                    SendPacketToServer( packet, time );
                }
            }
            break;

            case CLIENT_STATE_SENDING_CHALLENGE_RESPONSE:
            {
                if ( m_lastPacketSendTime + ConnectionResponseSendRate > time )
                    return;

                char buffer[256];
                const char *addressString = m_serverAddress.ToString( buffer, sizeof( buffer ) );
                printf( "client sending challenge response to server %s\n", addressString );

                ConnectionResponsePacket * packet = (ConnectionResponsePacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_RESPONSE );

                if ( packet )
                {
                    memcpy( packet->challengeTokenData, m_challengeTokenData, ChallengeTokenBytes );
                    memcpy( packet->challengeTokenNonce, m_challengeTokenNonce, NonceBytes );
                    
                    SendPacketToServer( packet, time );
                }
            }
            break;

            case CLIENT_STATE_CONNECTED:
            {
                if ( m_lastPacketSendTime + ConnectionHeartBeatRate > time )
                    return;

                ConnectionHeartBeatPacket * packet = (ConnectionHeartBeatPacket*) m_networkInterface->CreatePacket( PACKET_CONNECTION_HEARTBEAT );

                if ( packet )
                {
                    SendPacketToServer( packet, time );
                }
            }
            break;

            default:
                break;
        }
    }

    void Client::ReceivePackets( double time )
    {
        while ( true )
        {
            Address address;
            Packet *packet = m_networkInterface->ReceivePacket( address );
            if ( !packet )
                break;
            
            switch ( packet->GetType() )
            {
                case PACKET_CONNECTION_DENIED:
                    ProcessConnectionDenied( *(ConnectionDeniedPacket*)packet, address, time );
                    break;

                case PACKET_CONNECTION_CHALLENGE:
                    ProcessConnectionChallenge( *(ConnectionChallengePacket*)packet, address, time );
                    break;

                case PACKET_CONNECTION_HEARTBEAT:
                    ProcessConnectionHeartBeat( *(ConnectionHeartBeatPacket*)packet, address, time );
                    break;

                case PACKET_CONNECTION_DISCONNECT:
                    ProcessConnectionDisconnect( *(ConnectionDisconnectPacket*)packet, address, time );
                    break;

                default:
                    break;
            }

            m_networkInterface->DestroyPacket( packet );
        }
    }

    void Client::CheckForTimeOut( double time )
    {
        switch ( m_clientState )
        {
            case CLIENT_STATE_SENDING_CONNECTION_REQUEST:
            {
                if ( m_lastPacketReceiveTime + ConnectionRequestTimeOut < time )
                {
                    printf( "connection request to server timed out\n" );
                    m_clientState = CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT;
                    return;
                }
            }
            break;

            case CLIENT_STATE_SENDING_CHALLENGE_RESPONSE:
            {
                if ( m_lastPacketReceiveTime + ChallengeResponseTimeOut < time )
                {
                    printf( "challenge response to server timed out\n" );
                    m_clientState = CLIENT_STATE_CHALLENGE_RESPONSE_TIMED_OUT;
                    return;
                }
            }
            break;

            case CLIENT_STATE_CONNECTED:
            {
                if ( m_lastPacketReceiveTime + ConnectionTimeOut < time )
                {
                    printf( "keep alive timed out\n" );
                    m_clientState = CLIENT_STATE_CONNECTION_TIMED_OUT;
                    Disconnect( time );
                    return;
                }
            }
            break;

            default:
                break;
        }
    }

    void Client::ResetConnectionData()
    {
        assert( m_networkInterface );
        m_serverAddress = Address();
        m_clientState = CLIENT_STATE_DISCONNECTED;
        m_lastPacketSendTime = -1000.0;
        m_lastPacketReceiveTime = -1000.0;
        m_clientId = 0;
        memset( m_connectTokenData, 0, ConnectTokenBytes );
        memset( m_connectTokenNonce, 0, NonceBytes );
        memset( m_challengeTokenData, 0, ChallengeTokenBytes );
        memset( m_challengeTokenNonce, 0, NonceBytes );
        m_networkInterface->ResetEncryptionMappings();
    }

    void Client::SendPacketToServer( Packet *packet, double time )
    {
        assert( m_clientState != CLIENT_STATE_DISCONNECTED );
        assert( m_serverAddress.IsValid() );

        m_networkInterface->SendPacket( m_serverAddress, packet );

        m_lastPacketSendTime = time;
    }

    void Client::ProcessConnectionDenied( const ConnectionDeniedPacket & /*packet*/, const Address & address, double /*time*/ )
    {
        if ( m_clientState != CLIENT_STATE_SENDING_CONNECTION_REQUEST )
            return;

        if ( address != m_serverAddress )
            return;

        char buffer[256];
        const char * addressString = address.ToString( buffer, sizeof( buffer ) );
        printf( "client received connection denied from server: %s\n", addressString );
        m_clientState = CLIENT_STATE_CONNECTION_DENIED;
    }

    void Client::ProcessConnectionChallenge( const ConnectionChallengePacket & packet, const Address & address, double time )
    {
        if ( m_clientState != CLIENT_STATE_SENDING_CONNECTION_REQUEST )
            return;

        if ( address != m_serverAddress )
            return;

        char buffer[256];
        const char * addressString = address.ToString( buffer, sizeof( buffer ) );
        printf( "client received connection challenge from server: %s\n", addressString );

        memcpy( m_challengeTokenData, packet.challengeTokenData, ChallengeTokenBytes );
        memcpy( m_challengeTokenNonce, packet.challengeTokenNonce, NonceBytes );

        m_clientState = CLIENT_STATE_SENDING_CHALLENGE_RESPONSE;

        m_lastPacketReceiveTime = time;
    }

    void Client::ProcessConnectionHeartBeat( const ConnectionHeartBeatPacket & /*packet*/, const Address & address, double time )
    {
        if ( m_clientState < CLIENT_STATE_SENDING_CHALLENGE_RESPONSE )
            return;

        if ( address != m_serverAddress )
            return;

        if ( m_clientState == CLIENT_STATE_SENDING_CHALLENGE_RESPONSE )
        {
            char buffer[256];
            const char * addressString = address.ToString( buffer, sizeof( buffer ) );
            printf( "client is now connected to server: %s\n", addressString );

            memset( m_connectTokenData, 0, ConnectTokenBytes );
            memset( m_connectTokenNonce, 0, NonceBytes );
            memset( m_challengeTokenData, 0, ChallengeTokenBytes );
            memset( m_challengeTokenNonce, 0, NonceBytes );

            m_clientState = CLIENT_STATE_CONNECTED;        
        }

        m_lastPacketReceiveTime = time;
    }

    void Client::ProcessConnectionDisconnect( const ConnectionDisconnectPacket & /*packet*/, const Address & address, double time )
    {
        if ( m_clientState != CLIENT_STATE_CONNECTED )
            return;

        if ( address != m_serverAddress )
            return;

        Disconnect( time );
    }
}