package net.osmand.aidlapi.map;

import android.os.Bundle;
import android.os.Parcel;
import net.osmand.aidlapi.AidlParams;

public class NearbyRoadsParams extends AidlParams {

	private double centerLat;
	private double centerLon;
	private int radiusMeters;
	private int maxRoads;

	public NearbyRoadsParams() {
	}

	public NearbyRoadsParams(double centerLat, double centerLon, int radiusMeters, int maxRoads) {
		this.centerLat = centerLat;
		this.centerLon = centerLon;
		this.radiusMeters = radiusMeters;
		this.maxRoads = maxRoads;
	}

	protected NearbyRoadsParams(Parcel in) {
		readFromParcel(in);
	}

	public static final Creator<NearbyRoadsParams> CREATOR = new Creator<NearbyRoadsParams>() {
		@Override
		public NearbyRoadsParams createFromParcel(Parcel in) {
			return new NearbyRoadsParams(in);
		}

		@Override
		public NearbyRoadsParams[] newArray(int size) {
			return new NearbyRoadsParams[size];
		}
	};

	public double getCenterLat() { return centerLat; }
	public void setCenterLat(double v) { centerLat = v; }
	public double getCenterLon() { return centerLon; }
	public void setCenterLon(double v) { centerLon = v; }
	public int getRadiusMeters() { return radiusMeters; }
	public void setRadiusMeters(int v) { radiusMeters = v; }
	public int getMaxRoads() { return maxRoads; }
	public void setMaxRoads(int v) { maxRoads = v; }

	@Override
	protected void readFromBundle(Bundle bundle) {
		centerLat = bundle.getDouble("centerLat");
		centerLon = bundle.getDouble("centerLon");
		radiusMeters = bundle.getInt("radiusMeters");
		maxRoads = bundle.getInt("maxRoads");
	}

	@Override
	public void writeToBundle(Bundle bundle) {
		bundle.putDouble("centerLat", centerLat);
		bundle.putDouble("centerLon", centerLon);
		bundle.putInt("radiusMeters", radiusMeters);
		bundle.putInt("maxRoads", maxRoads);
	}
}
