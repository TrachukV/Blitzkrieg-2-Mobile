#pragma once

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template <class TParamType>
class CParam : public pair<TParamType,bool>
{
	typedef pair<TParamType,bool> TParent;
public:
	CParam() { this->second = false; }
	CParam( const TParamType &par ) : pair<TParamType,bool>( par, true ){  }
	const CParam &operator=( const TParamType &par ) { pair<TParamType,bool>::operator =( pair<TParamType,bool>( par, true ) ); return *this; }

	void Merge( const CParam &par )
	{
		if ( !IsValid() )
			pair<TParamType,bool>::operator=( par );
	}

	bool IsValid() const { return this->second; }
	TParamType &Get() { return this->first; }
	const TParamType &Get() const { return this->first; }
	operator TParamType() { return this->first; }
	int operator&( IBinSaver &f ) { f.Add( 1, &this->first ); f.Add( 2, &this->second ); return 0; }
	DWORD CalcCheckSum() const { return 0; }
};
